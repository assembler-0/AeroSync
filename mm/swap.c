/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/swap.c
 * @brief Swap subsystem implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * The swap subsystem extends memory capacity by swapping anonymous pages
 * to persistent storage. It integrates with ZMM: pages that compress well
 * go to ZMM, pages that don't compress well go to swap.
 *
 * Priority order for anonymous page reclaim:
 *   1. ZMM compression (fast, in-memory)
 *   2. Swap to SSD (slower, but unlimited capacity)
 *   3. OOM kill (last resort)
 */

#include <mm/swap.h>
#include <mm/zone.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/gfp.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/errno.h>
#include <aerosync/classes.h>

#ifdef CONFIG_MM_SWAP

/* Global swap state */
struct swap_info_struct *swap_info[MAX_SWAPFILES];
int nr_swapfiles = 0;
atomic_long_t total_swap_pages = {0};
atomic_long_t nr_swap_pages = {0};

static DEFINE_SPINLOCK(swap_lock);

/* Swap cache - simple hash table */
#define SWAP_CACHE_SIZE     1024
#define SWAP_CACHE_MASK     (SWAP_CACHE_SIZE - 1)

static struct {
  spinlock_t lock;
  struct list_head entries;
} swap_cache[SWAP_CACHE_SIZE];

static inline unsigned int swap_cache_hash(swp_entry_t entry) {
  return (swp_type(entry) ^ swp_offset(entry)) & SWAP_CACHE_MASK;
}

/**
 * swap_init - Initialize the swap subsystem
 */
int swap_init(void) {
  for (int i = 0; i < SWAP_CACHE_SIZE; i++) {
    spinlock_init(&swap_cache[i].lock);
    INIT_LIST_HEAD(&swap_cache[i].entries);
  }

  for (int i = 0; i < MAX_SWAPFILES; i++) {
    swap_info[i] = nullptr;
  }

  printk(KERN_INFO "swap: " "Swap subsystem initialized (max %d devices)\n",
         MAX_SWAPFILES);
  return 0;
}

/**
 * find_free_swap_slot - Find a swap device with free capacity
 */
static struct swap_info_struct *find_swap_device(void) {
  struct swap_info_struct *best = nullptr;
  int best_prio = -1;

  for (int i = 0; i < nr_swapfiles; i++) {
    struct swap_info_struct *si = swap_info[i];
    if (!si || !(si->flags & SWP_WRITEOK))
      continue;

    long free = atomic_long_read(&si->total_pages) -
                atomic_long_read(&si->inuse_pages);
    if (free <= 0)
      continue;

    if (si->prio > best_prio) {
      best = si;
      best_prio = si->prio;
    }
  }

  return best;
}

/**
 * scan_swap_map - Find a free slot in a swap device
 * @si: Swap device to search
 *
 * Returns the slot offset, or 0 on failure.
 */
static unsigned long scan_swap_map(struct swap_info_struct *si) {
  unsigned long offset;
  unsigned long scan_limit;

  if (!si || !si->swap_map)
    return 0;

  spin_lock(&si->lock);

  scan_limit = si->highest_bit - si->lowest_bit + 1;
  offset = si->cluster_next;

  /* Scan for a free slot */
  for (unsigned long i = 0; i < scan_limit; i++) {
    if (offset > si->highest_bit)
      offset = si->lowest_bit;

    if (si->swap_map[offset] == SWAP_MAP_FREE) {
      si->swap_map[offset] = 1;
      si->cluster_next = offset + 1;
      atomic_long_inc(&si->inuse_pages);
      atomic_long_dec(&nr_swap_pages);
      spin_unlock(&si->lock);
      return offset;
    }
    offset++;
  }

  spin_unlock(&si->lock);
  return 0; /* No free slots */
}

/**
 * get_swap_page - Allocate a swap slot for a folio
 * @folio: The folio to be swapped out
 *
 * Returns a swap entry, or a zero entry on failure.
 */
swp_entry_t get_swap_page(struct folio *folio) {
  swp_entry_t entry = {.val = 0};

  if (!swap_is_enabled())
    return entry;

  struct swap_info_struct *si = find_swap_device();
  if (!si)
    return entry;

  unsigned long offset = scan_swap_map(si);
  if (offset == 0)
    return entry;

  entry = swp_entry(si->type, offset);
  return entry;
}

/**
 * swap_free - Release a swap slot
 * @entry: The swap entry to free
 */
void swap_free(swp_entry_t entry) {
  if (non_swap_entry(entry))
    return;

  unsigned int type = swp_type(entry);
  unsigned long offset = swp_offset(entry);

  if (type >= MAX_SWAPFILES)
    return;

  struct swap_info_struct *si = swap_info[type];
  if (!si || !si->swap_map)
    return;

  spin_lock(&si->lock);

  if (offset <= si->highest_bit && si->swap_map[offset] > 0) {
    si->swap_map[offset]--;
    if (si->swap_map[offset] == SWAP_MAP_FREE) {
      atomic_long_dec(&si->inuse_pages);
      atomic_long_inc(&nr_swap_pages);
    }
  }

  spin_unlock(&si->lock);
}

/**
 * swap_duplicate - Increment reference count on a swap entry (for COW)
 * @entry: The swap entry to duplicate
 *
 * Returns 0 on success, negative on failure.
 */
int swap_duplicate(swp_entry_t entry) {
  if (non_swap_entry(entry))
    return -EINVAL;

  unsigned int type = swp_type(entry);
  unsigned long offset = swp_offset(entry);

  if (type >= MAX_SWAPFILES)
    return -EINVAL;

  struct swap_info_struct *si = swap_info[type];
  if (!si || !si->swap_map)
    return -EINVAL;

  spin_lock(&si->lock);

  if (offset <= si->highest_bit &&
      si->swap_map[offset] > 0 &&
      si->swap_map[offset] < SWAP_MAP_MAX) {
    si->swap_map[offset]++;
    spin_unlock(&si->lock);
    return 0;
  }

  spin_unlock(&si->lock);
  return -ENOENT;
}

/**
 * swap_writepage - Write a folio to swap
 * @folio: The folio to write
 * @entry: The swap entry (slot) to write to
 *
 * Returns 0 on success, negative on failure.
 */
int swap_writepage(struct folio *folio, swp_entry_t entry) {
  if (non_swap_entry(entry))
    return -EINVAL;

  unsigned int type = swp_type(entry);
  unsigned long offset = swp_offset(entry);

  if (type >= MAX_SWAPFILES)
    return -EINVAL;

  struct swap_info_struct *si = swap_info[type];
  if (!si)
    return -EINVAL;

  /*
   * NOTE: Actual I/O to block device requires integration with
   * the block layer (submit_bio).
   *
   * In a production implementation:
   *   1. Build a bio for the swap page
   *   2. Calculate the sector from offset
   *   3. Submit async I/O
   *   4. Handle completion callback
   */
  if (si->flags & SWP_SYNTHETIC) {
      /* Synthetic swap for testing: just mark as in cache */
      folio->flags |= (1UL << 20); /* PG_swapcache */
      return 0;
  }

  /* Real I/O would go here */
  return -EIO;

  return 0;
}

/**
 * swap_readpage - Read a folio from swap
 * @entry: The swap entry to read from
 *
 * Returns the folio on success, nullptr on failure.
 */
struct folio *swap_readpage(swp_entry_t entry) {
  if (non_swap_entry(entry))
    return nullptr;

  /* Check swap cache first */
  struct folio *folio = lookup_swap_cache(entry);
  if (folio)
    return folio;

  unsigned int type = swp_type(entry);
  if (type >= MAX_SWAPFILES)
    return nullptr;

  struct swap_info_struct *si = swap_info[type];
  if (!si)
    return nullptr;

  /* Allocate a new folio */
  folio = alloc_pages(GFP_KERNEL, 0);
  if (!folio)
    return nullptr;

  /*
   * NOTE: Actual I/O from block device requires bio submission.
   * Similar to swap_writepage, this would submit a read bio.
   */
  if (si->flags & SWP_SYNTHETIC) {
      /* Synthetic swap: return zeroed page (simulates successful read) */
      memset(folio_address(folio), 0, PAGE_SIZE);
  } else {
      /* Real I/O would go here */
      folio_put(folio);
      return nullptr;
  }

  /* Add to swap cache for concurrent access */
  if (add_to_swap_cache(folio, entry) != 0) {
    /* Another thread added it, use theirs */
    folio_put(folio);
    return lookup_swap_cache(entry);
  }

  return folio;
}

/**
 * lookup_swap_cache - Find a folio in the swap cache
 * @entry: The swap entry to look up
 */
struct folio *lookup_swap_cache(swp_entry_t entry) {
  unsigned int hash = swap_cache_hash(entry);

  spin_lock(&swap_cache[hash].lock);

  struct swap_cache_entry *sce;
  list_for_each_entry(sce, &swap_cache[hash].entries, list) {
    if (sce->entry.val == entry.val) {
      struct folio *folio = sce->folio;
      folio_get(folio);
      spin_unlock(&swap_cache[hash].lock);
      return folio;
    }
  }

  spin_unlock(&swap_cache[hash].lock);
  return nullptr;
}

/**
 * add_to_swap_cache - Add a folio to the swap cache
 * @folio: The folio to add
 * @entry: The swap entry
 *
 * Returns 0 on success, -EEXIST if already present.
 */
int add_to_swap_cache(struct folio *folio, swp_entry_t entry) {
  unsigned int hash = swap_cache_hash(entry);

  struct swap_cache_entry *sce = kmalloc(sizeof(*sce));
  if (!sce)
    return -ENOMEM;

  sce->entry = entry;
  sce->folio = folio;
  atomic_set(&sce->refcount, 1);

  spin_lock(&swap_cache[hash].lock);

  /* Check if already present */
  struct swap_cache_entry *existing;
  list_for_each_entry(existing, &swap_cache[hash].entries, list) {
    if (existing->entry.val == entry.val) {
      spin_unlock(&swap_cache[hash].lock);
      kfree(sce);
      return -EEXIST;
    }
  }

  list_add(&sce->list, &swap_cache[hash].entries);
  folio_get(folio); /* Cache holds a reference */

  spin_unlock(&swap_cache[hash].lock);
  return 0;
}

/**
 * delete_from_swap_cache - Remove a folio from the swap cache
 * @folio: The folio to remove
 */
void delete_from_swap_cache(struct folio *folio) {
  /*
   * This requires walking all cache buckets or storing the entry
   * in the folio. For now, we rely on the folio's private field
   * to store the swap entry.
   */
  if (!folio->private)
    return;

  swp_entry_t entry = *(swp_entry_t *) &folio->private;
  unsigned int hash = swap_cache_hash(entry);

  spin_lock(&swap_cache[hash].lock);

  struct swap_cache_entry *sce, *tmp;
  list_for_each_entry_safe(sce, tmp, &swap_cache[hash].entries, list) {
    if (sce->folio == folio) {
      list_del(&sce->list);
      folio_put(folio); /* Release cache's reference */
      kfree(sce);
      break;
    }
  }

  spin_unlock(&swap_cache[hash].lock);
}

/**
 * swap_cluster_readahead - Readahead a cluster of swap pages
 * @entry: The swap entry that triggered the fault
 * @gfp_mask: Allocation flags
 *
 * Returns the requested folio, with neighbors queued for async read.
 */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask) {
#ifdef CONFIG_MM_SWAP_READAHEAD
  unsigned long offset = swp_offset(entry);
  unsigned int type = swp_type(entry);

  /* Calculate cluster boundaries */
  unsigned long cluster_start = offset & ~(SWAP_CLUSTER_SIZE - 1);
  unsigned long cluster_end = cluster_start + SWAP_CLUSTER_SIZE;

  if (type >= MAX_SWAPFILES || !swap_info[type])
    return swap_readpage(entry);

  struct swap_info_struct *si = swap_info[type];
  if (cluster_end > si->highest_bit)
    cluster_end = si->highest_bit + 1;

  /* Read the cluster asynchronously */
  for (unsigned long off = cluster_start; off < cluster_end; off++) {
    if (off == offset)
      continue; /* We'll read this one synchronously */

    swp_entry_t ra_entry = swp_entry(type, off);

    /* Check if slot is in use */
    spin_lock(&si->lock);
    bool in_use = (si->swap_map[off] > 0);
    spin_unlock(&si->lock);

    if (in_use && !lookup_swap_cache(ra_entry)) {
      /* Queue async read (placeholder) */
      /* In reality, we'd submit a bio without waiting */
    }
  }
#endif

  /* Read the requested page synchronously */
  return swap_readpage(entry);
}

/**
 * sys_swapon - Enable a swap device
 * @path: Path to swap file/device
 * @flags: Swap flags (priority, etc.)
 *
 * Returns 0 on success, negative on failure.
 */
int sys_swapon(const char *path, int flags) {
  if (nr_swapfiles >= MAX_SWAPFILES)
    return -EPERM;

  /* Allocate swap_info_struct */
  struct swap_info_struct *si = kmalloc(sizeof(*si));
  if (!si)
    return -ENOMEM;

  memset(si, 0, sizeof(*si));
  spinlock_init(&si->lock);
  rwsem_init(&si->alloc_lock);
  INIT_LIST_HEAD(&si->free_clusters);
  INIT_LIST_HEAD(&si->partial_clusters);
  INIT_LIST_HEAD(&si->extent_list);

  si->prio = (flags >> 16) & 0x7FFF; /* Priority in high bits */
  if (si->prio == 0)
    si->prio = -1; /* Default priority */

  /* Copy path */
  size_t len = strlen(path);
  if (len >= sizeof(si->name))
    len = sizeof(si->name) - 1;
  memcpy(si->name, path, len);
  si->name[len] = '\0';

  /*
   * NOTE: Actual swap device initialization requires reading the swap header
   * from the block device or file.
   */
  if (flags & SWP_SYNTHETIC) {
      /* Create a synthetic swap device for testing/benchmarking */
      si->flags |= SWP_SYNTHETIC;
  }

  /* Placeholder: Allocate 256MB of swap (65536 pages) */
  unsigned long nr_pages = 65536;

  si->swap_map = kmalloc(nr_pages);
  if (!si->swap_map) {
    kfree(si);
    return -ENOMEM;
  }
  memset(si->swap_map, SWAP_MAP_FREE, nr_pages);

  si->lowest_bit = 1; /* Slot 0 is reserved */
  si->highest_bit = nr_pages - 1;
  si->cluster_next = 1;

  atomic_long_set(&si->total_pages, nr_pages - 1);
  atomic_long_set(&si->inuse_pages, 0);

  /* Register the swap device */
  spin_lock(&swap_lock);
  si->type = nr_swapfiles;
  swap_info[nr_swapfiles] = si;
  nr_swapfiles++;

  atomic_long_add(nr_pages - 1, &total_swap_pages);
  atomic_long_add(nr_pages - 1, &nr_swap_pages);

  si->flags = SWP_USED | SWP_WRITEOK | SWP_SYNTHETIC;
  spin_unlock(&swap_lock);

  printk(KERN_INFO "swap: " "Activated swap: %s (%lu pages, priority %d)\n",
         si->name, nr_pages - 1, si->prio);

  return 0;
}

/**
 * sys_swapoff - Disable a swap device
 * @path: Path to the swap file/device
 *
 * Returns 0 on success, negative on failure.
 */
int sys_swapoff(const char *path) {
  struct swap_info_struct *si = nullptr;

  spin_lock(&swap_lock);
  for (int i = 0; i < nr_swapfiles; i++) {
    if (swap_info[i] && strcmp(swap_info[i]->name, path) == 0) {
      si = swap_info[i];
      break;
    }
  }
  spin_unlock(&swap_lock);

  if (!si)
    return -ENOENT;

  /* Check if any pages are still swapped */
  if (atomic_long_read(&si->inuse_pages) > 0) {
    /*
     * TODO: Migrate all swapped pages back to RAM.
     * This requires walking all address spaces and swapping in pages.
     */
    return -EBUSY;
  }

  spin_lock(&swap_lock);
  si->flags &= ~SWP_WRITEOK;

  atomic_long_sub(atomic_long_read(&si->total_pages), &total_swap_pages);
  atomic_long_sub(atomic_long_read(&si->total_pages) -
                  atomic_long_read(&si->inuse_pages), &nr_swap_pages);
  spin_unlock(&swap_lock);

  /* Free resources */
  kfree(si->swap_map);
  kfree(si);

  printk(KERN_INFO "swap: " "Deactivated swap: %s\n", path);

  return 0;
}

#else /* !CONFIG_MM_SWAP */

/* Stubs when swap is disabled */
int swap_init(void) { return 0; }
swp_entry_t get_swap_page(struct folio *folio) {
  (void) folio;
  return (swp_entry_t){.val = 0};
}
void swap_free(swp_entry_t entry) { (void) entry; }
int swap_duplicate(swp_entry_t entry) {
  (void) entry;
  return -ENOSYS;
}
int swap_writepage(struct folio *folio, swp_entry_t entry) {
  (void) folio;
  (void) entry;
  return -ENOSYS;
}
struct folio *swap_readpage(swp_entry_t entry) {
  (void) entry;
  return nullptr;
}
struct folio *lookup_swap_cache(swp_entry_t entry) {
  (void) entry;
  return nullptr;
}
int add_to_swap_cache(struct folio *folio, swp_entry_t entry) {
  (void) folio;
  (void) entry;
  return -ENOSYS;
}
void delete_from_swap_cache(struct folio *folio) { (void) folio; }
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask) {
  (void) entry;
  (void) gfp_mask;
  return nullptr;
}
int sys_swapon(const char *path, int flags) {
  (void) path;
  (void) flags;
  return -ENOSYS;
}
int sys_swapoff(const char *path) {
  (void) path;
  return -ENOSYS;
}

struct swap_info_struct *swap_info[MAX_SWAPFILES];
int nr_swapfiles = 0;
atomic_long_t total_swap_pages = ATOMIC_LONG_INIT(0);
atomic_long_t nr_swap_pages = ATOMIC_LONG_INIT(0);

#endif /* CONFIG_MM_SWAP */
