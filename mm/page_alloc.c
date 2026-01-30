///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/page_alloc.c
 * @brief Zone allocator
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <mm/zone.h>
#include <mm/page.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <aerosync/spinlock.h>
#include <aerosync/panic.h>
#include <arch/x86_64/cpu.h>
#include <aerosync/classes.h>
#include <linux/list.h>
#include <linux/container_of.h>
#include <mm/gfp.h>
#include <aerosync/sched/sched.h>
#include <lib/math.h>
#include <lib/string.h>

#define PAGE_POISON_FREE  0xfe
#define PAGE_POISON_ALLOC 0xad

static void kernel_poison_pages(struct page *page, int numpages, uint8_t val) {
#ifdef MM_HARDENING
  void *addr = page_address(page);
  memset(addr, val, (size_t) numpages << PAGE_SHIFT);
#else
  (void) page; (void) numpages; (void) val;
#endif
}

static void check_page_poison(struct page *page, int numpages) {
#ifdef MM_HARDENING
  uint64_t *p = (uint64_t *) page_address(page);
  size_t count = (size_t) numpages << (PAGE_SHIFT - 3);
  uint64_t expected = 0xfefefefefefefefeULL;

  for (size_t i = 0; i < count; i++) {
    if (unlikely(p[i] != expected)) {
      /* Fallback to byte-by-byte to find the exact corrupt byte for the panic message */
      uint8_t *byte_p = (uint8_t *) p;
      size_t byte_size = (size_t) numpages << PAGE_SHIFT;
      for (size_t j = 0; j < byte_size; j++) {
        if (byte_p[j] != PAGE_POISON_FREE) {
          panic(PMM_CLASS "Page poisoning corruption detected at %p (offset %zu, val 0x%02x)\n",
                byte_p, j, byte_p[j]);
        }
      }
    }
  }
#else
  (void) page; (void) numpages;
#endif
}

/* Global zones */
struct zone managed_zones[MAX_NR_ZONES];

/* Default zone names */
static const char *const zone_names[MAX_NR_ZONES] = {
  "DMA",
  "DMA32",
  "Normal"
};

/* Defined in arch/x86_64/mm/pmm.c */
extern struct page *mem_map;
extern uint64_t pmm_max_pages;

extern void wakeup_kswapd(struct zone *zone);

extern size_t try_to_free_pages(struct pglist_data *pgdat, size_t nr_to_reclaim, gfp_t gfp_mask);

/*
 * Debugging helper
 */
static void check_page_sanity(struct page *page, int order) {
  if (PageBuddy(page)) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Bad page state: PageBuddy set in alloc path (pfn %llu)",
             (uint64_t) (page - mem_map));
    panic(buf);
  }
  if (page->order != 0 && page->order != order) {
    page->order = 0;
  }
}

/*
 * Buddy System Core
 */

static inline unsigned long __find_buddy_pfn(unsigned long page_pfn, unsigned int order) {
  return page_pfn ^ (1UL << order);
}

static inline bool page_is_buddy(struct page *page, struct page *buddy, unsigned int order) {
  if (!PageBuddy(buddy))
    return false;
  if (buddy->order != order)
    return false;
  return true;
}

/*
 * Fallback table for migration types.
 * Defines which migration types can be borrowed from when a specific type is empty.
 */
static int fallbacks[MIGRATE_TYPES][4] = {
  [MIGRATE_UNMOVABLE] = {MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE, MIGRATE_TYPES},
  [MIGRATE_RECLAIMABLE] = {MIGRATE_UNMOVABLE, MIGRATE_MOVABLE, MIGRATE_TYPES},
  [MIGRATE_MOVABLE] = {MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_TYPES},
};

static inline void expand(struct zone *zone, struct page *page,
                          int low, int high, struct free_area *area,
                          int migratetype) {
  unsigned long size = 1UL << high;

  while (high > low) {
    area--;
    high--;
    size >>= 1;

    struct page *buddy = page + size;
    INIT_LIST_HEAD(&buddy->list);

    SetPageBuddy(buddy);
    buddy->order = high;
    buddy->migratetype = migratetype;

    list_add(&buddy->list, &area->free_list[migratetype]);
    area->nr_free++;
    zone->nr_free_pages += size;
    if (high > (int) zone->max_free_order) zone->max_free_order = high;
  }
}

static struct page *__rmqueue_fallback(struct zone *zone, unsigned int order,
                                       int start_migratetype) {
  struct free_area *area;
  unsigned int current_order;
  int i;
  int migratetype;

  /* Find the largest possible block of any fallback type */
  for (current_order = MAX_ORDER - 1; current_order >= order; current_order--) {
    for (i = 0; i < MIGRATE_TYPES; i++) {
      migratetype = fallbacks[start_migratetype][i];
      if (migratetype == MIGRATE_TYPES) break;

      area = &zone->free_area[current_order];
      if (list_empty(&area->free_list[migratetype]))
        continue;

      struct page *page = list_entry(area->free_list[migratetype].next, struct page, list);
      list_del(&page->list);

      ClearPageBuddy(page);
      area->nr_free--;
      zone->nr_free_pages -= (1UL << current_order);

      /* When borrowing from another type, move all objects if it's a large block */
      if (current_order >= (MAX_ORDER / 2)) {
        // ... could implement move_freepages_block here ...
      }

      expand(zone, page, order, current_order, area, start_migratetype);
      page->migratetype = start_migratetype;
      return page;
    }
  }

  return nullptr;
}

static struct page *__rmqueue(struct zone *zone, unsigned int order, int migratetype) {
  unsigned int current_order;
  struct free_area *area;
  struct page *page;

  for (current_order = order; current_order < MAX_ORDER; ++current_order) {
    area = &zone->free_area[current_order];
    if (list_empty(&area->free_list[migratetype]))
      continue;

    page = list_entry(area->free_list[migratetype].next, struct page, list);
    list_del(&page->list);

    ClearPageBuddy(page);
    area->nr_free--;
    zone->nr_free_pages -= (1UL << current_order);

    /* Update max_free_order if we just allocated the last block of that order */
    if (current_order == zone->max_free_order && area->nr_free == 0) {
      int o = current_order;
      while (o > 0 && zone->free_area[o].nr_free == 0) o--;
      zone->max_free_order = o;
    }

    expand(zone, page, order, current_order, area, migratetype);
    page->migratetype = migratetype;
    return page;
  }

  return __rmqueue_fallback(zone, order, migratetype);
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order,
                            int migratetype);

int rmqueue_bulk(struct zone *zone, unsigned int order, unsigned int count,
                 struct list_head *list, int migratetype) {
  int i;

  /* Validate zone boundaries before bulk allocation */
  if (!zone || !zone->present_pages || count == 0) return 0;

  spinlock_lock(&zone->lock);

  for (i = 0; i < (int) count; ++i) {
    struct page *page = __rmqueue(zone, order, migratetype);
    if (unlikely(page == nullptr))
      break;

    list_add_tail(&page->list, list);
  }

  spinlock_unlock(&zone->lock);
  return i;
}

/**
 * drain_zone_pages - Return a batch of pages from a PCP list to the buddy system.
 * This is the core of the "Batched PCP" optimization.
 */
static void drain_zone_pages(struct zone *zone, struct list_head *list, int count, int order) {
  unsigned long flags;
  struct page *page;

  flags = spinlock_lock_irqsave(&zone->lock);

  while (count-- > 0 && !list_empty(list)) {
    page = list_first_entry(list, struct page, list);
    list_del(&page->list);
    __free_one_page(page, (unsigned long) (page - mem_map), zone, order, page->migratetype);
  }

  spinlock_unlock_irqrestore(&zone->lock, flags);
}

void free_pcp_pages(struct zone *zone, int count, struct list_head *list, int order) {
  drain_zone_pages(zone, list, count, order);
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order,
                            int migratetype) {
  unsigned long buddy_pfn;
  struct page *buddy;
  unsigned long combined_pfn;

  // Validate page before starting merge process
  if (unlikely(PageBuddy(page))) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free detected: pfn %lu", pfn);
    panic(buf);
  }

  while (order < MAX_ORDER - 1) {
    buddy_pfn = __find_buddy_pfn(pfn, order);
    buddy = page + (long) (buddy_pfn - pfn);

    /*
     * Fast buddy check:
     * 1. Buddy must be within the same zone.
     * 2. Buddy must be on a buddy list.
     * 3. Buddy must have the same order.
     */
    if (!page_is_buddy(page, buddy, order))
      break;

    /* Our buddy is free, merge with it */
    list_del(&buddy->list);
    zone->free_area[order].nr_free--;
    zone->nr_free_pages -= (1UL << order);
    ClearPageBuddy(buddy);
    buddy->order = 0;

    combined_pfn = buddy_pfn & pfn;
    page = page + (long) (combined_pfn - pfn);
    pfn = combined_pfn;
    order++;
  }

  SetPageBuddy(page);
  page->order = order;
  page->migratetype = migratetype;
  list_add(&page->list, &zone->free_area[order].free_list[migratetype]);
  zone->free_area[order].nr_free++;
  zone->nr_free_pages += (1UL << order);
  if (order > zone->max_free_order) zone->max_free_order = order;
}

extern size_t shrink_inactive_list(size_t nr_to_scan);

static inline int gfp_to_migratetype(gfp_t gfp_mask) {
  if (gfp_mask & ___GFP_MOVABLE)
    return MIGRATE_MOVABLE;
  if (gfp_mask & ___GFP_RECLAIMABLE)
    return MIGRATE_RECLAIMABLE;
  return MIGRATE_UNMOVABLE;
}

extern int numa_distance_get(int from, int to);

/*
 * Zonelist Construction
 */
static void build_zonelists_node(struct pglist_data *pgdat, struct zonelist *zonelist, int nr_zones) {
  int i, z;
  int zone_idx = 0;

  /* 1. Local zones (Highest to Lowest) */
  for (z = nr_zones; z >= 0; z--) {
    struct zone *zone = &pgdat->node_zones[z];
    if (zone->present_pages) {
      zonelist->_zones[zone_idx++] = zone;
    }
  }

  /* 2. Remote zones by distance */
  int visited[MAX_NUMNODES] = {0};
  visited[pgdat->node_id] = 1;

  for (int count = 0; count < MAX_NUMNODES - 1; count++) {
    int best_node = -1;
    int min_dist = 256;

    for (i = 0; i < MAX_NUMNODES; i++) {
      if (!node_data[i] || visited[i]) continue;
      int dist = numa_distance_get(pgdat->node_id, i);
      /* Use 255 (NUMA_NO_DISTANCE) as max, so < checks work */
      if (dist < min_dist) {
        min_dist = dist;
        best_node = i;
      }
    }

    if (best_node == -1) break;
    visited[best_node] = 1;

    struct pglist_data *remote_pgdat = node_data[best_node];
    for (z = nr_zones; z >= 0; z--) {
      struct zone *zone = &remote_pgdat->node_zones[z];
      if (zone->present_pages) {
        zonelist->_zones[zone_idx++] = zone;
      }
    }
  }

  zonelist->_zones[zone_idx] = nullptr;
}

void build_all_zonelists(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;

    for (int z = 0; z < MAX_NR_ZONES; z++) {
      build_zonelists_node(node_data[n], &node_data[n]->node_zonelists[z], z);
    }
  }
  printk(KERN_INFO PMM_CLASS "Built zonelists for all nodes.\n");
}

/*
 * Core Allocator
 */
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  struct page *page = nullptr;
  struct pglist_data *pgdat = nullptr;
  struct zone *z;
  unsigned long flags;
  bool can_reclaim = !(gfp_mask & GFP_ATOMIC);
  int reclaim_retries = 3;
  int migratetype = gfp_to_migratetype(gfp_mask);

retry:
  if (nid < 0 || nid >= MAX_NUMNODES || !node_data[nid]) {
    /* Fallback to first valid node */
    nid = -1;
    for (int i = 0; i < MAX_NUMNODES; i++) {
      if (node_data[i]) {
        nid = i;
        break;
      }
    }
    if (nid == -1) {
      printk(KERN_ERR PMM_CLASS "No valid NUMA nodes available\n");
      return nullptr;
    }
  }

  int start_zone_idx = ZONE_NORMAL;
  if (gfp_mask & ___GFP_DMA) start_zone_idx = ZONE_DMA;
  else if (gfp_mask & ___GFP_DMA32) start_zone_idx = ZONE_DMA32;

  pgdat = node_data[nid];

  /*
   * PCP Fastpath (Orders 0-3, Local Node)
   * Only if we are requesting from the local node and not a specific remote node.
   * If nid != this_node(), we skip PCP to avoid locking remote zones without zone lock?
   * PCP is per-zone, per-cpu. We can only access THIS cpu's PCP for the zone.
   * So if the zone is on another node, we CAN access it, but we are accessing
   * the PCP structure for THIS cpu on that remote zone. This is valid.
   */
  if (order < PCP_ORDERS && percpu_ready()) {
    /*
     * We try the highest allowed zone in the local node first for PCP.
     * Usually ZONE_NORMAL.
     */
    z = &pgdat->node_zones[start_zone_idx];
    if (z->present_pages) {
      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int) smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

      if (list_empty(&pcp->lists[order])) {
        if (z->nr_free_pages >= z->watermark[WMARK_LOW]) {
          int count = rmqueue_bulk(z, order, pcp->batch, &pcp->lists[order], migratetype);
          pcp->count += count;
        }
      }

      if (!list_empty(&pcp->lists[order])) {
        page = list_first_entry(&pcp->lists[order], struct page, list);
        list_del(&page->list);
        pcp->count--;

        /* Verify and poison */
        check_page_poison(page, 1 << order);
        kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);

        struct folio *folio = (struct folio *) page;
        folio->order = (uint16_t) order;
        folio->node = page->node;
        folio->zone = page->zone;
        SetPageHead(&folio->page);
        atomic_set(&folio->_refcount, 1);

        restore_irq_flags(irq_flags);
        return folio;
      }
      restore_irq_flags(irq_flags);
    }
  }

  /*
   * Zonelist Traversal (The "Magnum" Path)
   */
  struct zonelist *zonelist = &pgdat->node_zonelists[start_zone_idx];
  struct zone **z_ptr = zonelist->_zones;

  while ((z = *z_ptr++) != nullptr) {
    if (!z->present_pages || order > z->max_free_order) continue;

    /* Check watermarks with atomic operations */
    if (__atomic_load_n(&z->nr_free_pages, __ATOMIC_ACQUIRE) < z->watermark[WMARK_LOW]) {
      wakeup_kswapd(z);
    }

    if (__atomic_load_n(&z->nr_free_pages, __ATOMIC_ACQUIRE) < z->watermark[WMARK_MIN] &&
        !can_reclaim && !(gfp_mask & ___GFP_HIGH)) {
      continue;
    }

    flags = spinlock_lock_irqsave(&z->lock);
    page = __rmqueue(z, order, migratetype);
    spinlock_unlock_irqrestore(&z->lock, flags);

    if (page) goto found;
  }

  /*
   * Direct Reclaim / Demand Allocation
   */
  if (can_reclaim && reclaim_retries > 0) {
    size_t reclaimed = try_to_free_pages(pgdat, 32, gfp_mask);
    if (reclaimed > 0) {
      reclaim_retries--;
      goto retry;
    }
  }

  printk(KERN_ERR PMM_CLASS "failed to allocate order %u from any node (gfp: %x)\n", order, gfp_mask);
  return nullptr;

found:
  check_page_sanity(page, order);

  /* Verify the page wasn't corrupted while on the free list */
  check_page_poison(page, 1 << order);
  /* Poison as allocated */
  kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);

  struct folio *folio = (struct folio *) page;

  folio->order = (uint16_t) order;
  folio->node = page->node; /* Use actual page node */
  folio->zone = page->zone;
  SetPageHead(&folio->page);

  if (order > 0) {
    size_t nr = 1UL << order;
    for (size_t i = 1; i < nr; i++) {
      struct page *tail = page + i;
      tail->flags = 0;
      SetPageTail(tail);
      tail->head = page;
      tail->node = page->node;
      tail->migratetype = page->migratetype;
    }
  }

  atomic_set(&folio->_refcount, 1);
  return folio;
}

unsigned long alloc_pages_bulk_array(int nid, gfp_t gfp_mask, unsigned int order,
                                     unsigned long nr_pages, struct page **pages_array) {
  if (!nr_pages) return 0;
  if (nid < 0) nid = 0; // fallback default
  if (!node_data[nid]) {
    /* find valid node */
    for (int i = 0; i < MAX_NUMNODES; i++) if (node_data[i]) {
      nid = i;
      break;
    }
  }

  struct pglist_data *pgdat = node_data[nid];
  struct zone *z;
  unsigned long allocated = 0;
  unsigned long flags;
  int migratetype = gfp_to_migratetype(gfp_mask);

  /* Fast Path: PCP */
  if (order < PCP_ORDERS && percpu_ready()) {
    int zone_idx = ZONE_NORMAL;
    if (gfp_mask & ___GFP_DMA) zone_idx = ZONE_DMA;

    z = &pgdat->node_zones[zone_idx];
    if (z->present_pages) {
      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int) smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

      while (allocated < nr_pages) {
        if (list_empty(&pcp->lists[order])) {
          /* Refill PCP */
          int batch = max((int)pcp->batch, (int)(nr_pages - allocated));
          if (batch > pcp->high) batch = pcp->high;

          int count = rmqueue_bulk(z, order, batch, &pcp->lists[order], migratetype);
          pcp->count += count;
          if (list_empty(&pcp->lists[order])) break; // Zone empty
        }

        struct page *page = list_first_entry(&pcp->lists[order], struct page, list);
        list_del(&page->list);
        pcp->count--;

        struct folio *folio = (struct folio *) page;
        folio->order = (uint16_t) order;
        folio->node = page->node;
        folio->zone = page->zone;
        SetPageHead(&folio->page);
        atomic_set(&folio->_refcount, 1);

        pages_array[allocated++] = page;
      }
      restore_irq_flags(irq_flags);
    }
  }

  if (allocated == nr_pages) return allocated;

  /* Slow Path: Zone Lock */
  // We just pick the Normal zone for now.
  z = &pgdat->node_zones[ZONE_NORMAL];
  if (!z->present_pages) z = &pgdat->node_zones[ZONE_DMA32];

  if (z->present_pages) {
    flags = spinlock_lock_irqsave(&z->lock);
    while (allocated < nr_pages) {
      struct page *page = __rmqueue(z, order, migratetype);
      if (!page) break;

      check_page_sanity(page, order);
      struct folio *folio = (struct folio *) page;
      folio->order = (uint16_t) order;
      folio->node = page->node;
      folio->zone = page->zone;
      SetPageHead(&folio->page);

      /* Handle higher orders tail pages if needed, omitted for bulk 0-order opt */
      if (order > 0) {
        size_t nr = 1UL << order;
        for (size_t i = 1; i < nr; i++) {
          struct page *tail = page + i;
          tail->flags = 0;
          SetPageTail(tail);
          tail->head = page;
          tail->node = page->node;
          tail->migratetype = page->migratetype;
        }
      }

      atomic_set(&folio->_refcount, 1);
      pages_array[allocated++] = page;
    }
    spinlock_unlock_irqrestore(&z->lock, flags);
  }

  return allocated;
}

void free_pages_bulk_array(unsigned long nr_pages, struct page **pages) {
  if (!nr_pages) return;

  for (unsigned long i = 0; i < nr_pages; i++) {
    struct page *page = pages[i];
    if (!page) continue;

    int order = page->order;
    struct zone *z = &managed_zones[page->zone]; // Fallback if node not accessible
    // Better: use page->node
    if (node_data[page->node]) {
      z = &node_data[page->node]->node_zones[page->zone];
    }

    /* Try PCP first */
    if (order < PCP_ORDERS && percpu_ready()) {
      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int) smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

      list_add(&page->list, &pcp->lists[order]);
      pcp->count++;

      if (pcp->count >= pcp->high) {
        drain_zone_pages(z, &pcp->lists[order], pcp->batch, order);
        pcp->count -= pcp->batch;
      }
      restore_irq_flags(irq_flags);
      continue;
    }

    /* Slow path */
    irq_flags_t flags = spinlock_lock_irqsave(&z->lock);
    __free_one_page(page, (unsigned long) (page - mem_map), z, order, page->migratetype);
    spinlock_unlock_irqrestore(&z->lock, flags);
  }
}

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order) {
  int nid = 0;
  if (current) nid = current->node_id;

  return alloc_pages_node(nid, gfp_mask, order);
}

void put_page(struct page *page) {
  if (!page || PageReserved(page)) return;

  struct folio *folio = page_folio(page);

  if (atomic_dec_and_test(&folio->_refcount)) {
    unsigned int order = 0;
    if (PageHead(&folio->page)) {
      order = folio->order;
      ClearPageHead(&folio->page);

      /* Cleanup tail pages */
      if (order > 0) {
        size_t nr = 1UL << order;
        for (size_t i = 1; i < nr; i++) {
          struct page *tail = &folio->page + i;
          ClearPageTail(tail);
          tail->head = nullptr;
        }
      }
    }
    __free_pages(&folio->page, order);
  }
}

void __free_pages(struct page *page, unsigned int order) {
  if (!page) return;

  if (unlikely(PageBuddy(page))) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free of page %p", page);
    panic(buf);
  }

  /* Poison the page being freed */
  kernel_poison_pages(page, 1 << order, PAGE_POISON_FREE);

  /*
   * PCP optimization for small orders (0-3).
   * This is the hot path for single page frees.
   */
  if (order < PCP_ORDERS && percpu_ready()) {
    irq_flags_t flags = save_irq_flags();
    int cpu = (int) smp_get_id();

    struct pglist_data *pgdat = node_data[page->node];
    struct zone *zone = &pgdat->node_zones[page->zone];
    struct per_cpu_pages *pcp = &zone->pageset[cpu];

    /* Always add to the head (HOT page) */
    list_add(&page->list, &pcp->lists[order]);
    pcp->count++;

    if (pcp->count >= pcp->high) {
      /* Drain half the batch to reduce lock acquisition frequency */
      int to_drain = pcp->batch;
      if (to_drain > pcp->count) to_drain = pcp->count;

      drain_zone_pages(zone, &pcp->lists[order], to_drain, order);
      pcp->count -= to_drain;
    }

    restore_irq_flags(flags);
    return;
  }

  struct pglist_data *pgdat = node_data[page->node];
  if (!pgdat) pgdat = node_data[0];
  struct zone *zone = &pgdat->node_zones[page->zone];

  unsigned long pfn = (unsigned long) (page - mem_map);
  unsigned long flags;
  flags = spinlock_lock_irqsave(&zone->lock);
  __free_one_page(page, pfn, zone, order, page->migratetype);
  spinlock_unlock_irqrestore(&zone->lock, flags);
}

void pmm_verify(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;
    struct pglist_data *pgdat = node_data[n];

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      if (!z->present_pages) continue;

      irq_flags_t flags = spinlock_lock_irqsave(&z->lock);
      unsigned long found_free = 0;

      for (int order = 0; order < MAX_ORDER; order++) {
        for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
          struct page *page;
          list_for_each_entry(page, &z->free_area[order].free_list[mt], list) {
            if (unlikely(!PageBuddy(page))) {
              panic("PMM: Page in free list without PageBuddy set! (pfn %lu)\n",
                    (unsigned long) (page - mem_map));
            }
            if (unlikely(page->order != order)) {
              panic("PMM: Page in free list with wrong order! (expected %d, got %d)\n",
                    order, page->order);
            }
            found_free += (1UL << order);
          }
        }
      }

      if (unlikely(found_free != z->nr_free_pages)) {
        panic("PMM: Free page count mismatch in zone %s! (found %lu, expected %lu)\n",
              z->name, found_free, z->nr_free_pages);
      }
      spinlock_unlock_irqrestore(&z->lock, flags);
    }
  }
}

void free_area_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;

    struct pglist_data *pgdat = node_data[n];
    init_waitqueue_head(&pgdat->kswapd_wait);
    pgdat->kswapd_task = nullptr;

    spinlock_init(&pgdat->lru_lock);
    for (int gen = 0; gen < MAX_NR_GENS; gen++) {
      for (int type = 0; type < 2; type++) {
        INIT_LIST_HEAD(&pgdat->lrugen.lists[gen][type]);
        atomic_long_set(&pgdat->lrugen.nr_pages[gen][type], 0);
      }
    }
    pgdat->lrugen.max_seq = 0;
    pgdat->lrugen.min_seq[0] = 0;
    pgdat->lrugen.min_seq[1] = 0;
    atomic_set(&pgdat->lrugen.gen_counter, 0);

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      spinlock_init(&z->lock);
      z->name = zone_names[i];
      z->zone_pgdat = pgdat;
      z->present_pages = 0;
      z->spanned_pages = 0;
      z->zone_start_pfn = 0;
      z->nr_free_pages = 0;
      z->max_free_order = 0;

      for (int order = 0; order < MAX_ORDER; order++) {
        for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
          INIT_LIST_HEAD(&z->free_area[order].free_list[mt]);
        }
        z->free_area[order].nr_free = 0;
      }
    }
  }
}
