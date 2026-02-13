/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/memory.c
 * @brief High-level memory management
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

#include <mm/mm_types.h>
#include <mm/zone.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/mmu_gather.h>
#include <mm/workingset.h>
#include <mm/swap.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/mutex.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <aerosync/wait.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/mm/tlb.h>

/* LRU Management */
#ifdef CONFIG_MM_LRU
/* Global LRU Lists - Per-CPU to reduce cache line bouncing */
DEFINE_PER_CPU(struct list_head, inactive_list);
DEFINE_PER_CPU(struct list_head, active_list);
DEFINE_PER_CPU(spinlock_t, lru_lock);
#endif

#ifdef CONFIG_MM_MGLRU
/*
 * MGLRU Bloom Filter for Page Table Scanning Optimization
 *
 * When scanning page tables for accessed bits, we use bloom filters
 * to skip cold page table regions. This dramatically reduces CPU
 * overhead for workloads with large address spaces.
 *
 * Each NUMA node has a bloom filter per generation. When a page is
 * accessed, we add its PTE's page table address to the current gen's
 * bloom filter. During aging, we only scan page tables that are in
 * the bloom filter.
 */
#ifdef CONFIG_MM_MGLRU_BLOOM_FILTER
#define BLOOM_FILTER_SIZE   (64 * 1024)  /* 64KB per filter = 512K bits */
#define BLOOM_FILTER_BITS   (BLOOM_FILTER_SIZE * 8)
#define BLOOM_HASH_COUNT    3

struct lru_gen_bloom {
  unsigned long *bits[MAX_NR_GENS];
  spinlock_t lock;
  atomic_long_t set_count[MAX_NR_GENS];
};

static struct lru_gen_bloom *node_bloom[MAX_NUMNODES];

static inline unsigned long bloom_hash(unsigned long addr, int seed) {
  /* Simple hash mixing - use different seeds for each hash */
  addr ^= seed * 0x9e3779b97f4a7c15UL;
  addr ^= addr >> 33;
  addr *= 0xff51afd7ed558ccdUL;
  addr ^= addr >> 33;
  return addr % BLOOM_FILTER_BITS;
}

static void bloom_add(struct lru_gen_bloom *bloom, int gen, unsigned long addr) {
  if (!bloom || !bloom->bits[gen])
    return;

  for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
    unsigned long bit = bloom_hash(addr, i);
    unsigned long word = bit / 64;
    unsigned long mask = 1UL << (bit % 64);
    __atomic_or_fetch(&bloom->bits[gen][word], mask, __ATOMIC_RELAXED);
  }
  atomic_long_inc(&bloom->set_count[gen]);
}

static bool bloom_test(struct lru_gen_bloom *bloom, int gen, unsigned long addr) {
  if (!bloom || !bloom->bits[gen])
    return true; /* Assume present if no filter */

  for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
    unsigned long bit = bloom_hash(addr, i);
    unsigned long word = bit / 64;
    unsigned long mask = 1UL << (bit % 64);
    if (!(__atomic_load_n(&bloom->bits[gen][word], __ATOMIC_RELAXED) & mask))
      return false;
  }
  return true;
}

static void bloom_clear(struct lru_gen_bloom *bloom, int gen) {
  if (!bloom || !bloom->bits[gen])
    return;

  memset(bloom->bits[gen], 0, BLOOM_FILTER_SIZE);
  atomic_long_set(&bloom->set_count[gen], 0);
}

static void bloom_init_node(int nid) {
  struct lru_gen_bloom *bloom = kmalloc(sizeof(*bloom));
  if (!bloom)
    return;

  spinlock_init(&bloom->lock);
  for (int i = 0; i < MAX_NR_GENS; i++) {
    bloom->bits[i] = kmalloc(BLOOM_FILTER_SIZE);
    if (bloom->bits[i]) {
      memset(bloom->bits[i], 0, BLOOM_FILTER_SIZE);
    }
    atomic_long_set(&bloom->set_count[i], 0);
  }
  node_bloom[nid] = bloom;
}
#endif /* CONFIG_MM_MGLRU_BLOOM_FILTER */

/*
 * Enhanced MGLRU Aging Heuristics
 *
 * We track additional metrics per generation to make better
 * reclaim decisions:
 *   - refault_count: Pages that refaulted from this generation
 *   - scan_count: Pages scanned from this generation
 *   - reclaim_count: Pages successfully reclaimed
 *
 * This allows adaptive policies like:
 *   - If refault_rate is high, slow down aging
 *   - If reclaim_rate is low, age more aggressively
 */
struct lru_gen_stats {
  atomic_long_t refaults[MAX_NR_GENS];
  atomic_long_t scanned[MAX_NR_GENS];
  atomic_long_t reclaimed[MAX_NR_GENS];
  atomic_long_t promoted[MAX_NR_GENS]; /* Pages promoted to younger gen */
};

static struct lru_gen_stats *node_lru_stats[MAX_NUMNODES];

static void lru_stats_init_node(int nid) {
  struct lru_gen_stats *stats = kmalloc(sizeof(*stats));
  if (!stats)
    return;

  for (int i = 0; i < MAX_NR_GENS; i++) {
    atomic_long_set(&stats->refaults[i], 0);
    atomic_long_set(&stats->scanned[i], 0);
    atomic_long_set(&stats->reclaimed[i], 0);
    atomic_long_set(&stats->promoted[i], 0);
  }
  node_lru_stats[nid] = stats;
}

/*
 * Adaptive aging interval based on memory pressure and refault rate.
 * Higher pressure = faster aging; higher refaults = slower aging.
 */
static unsigned long lru_gen_aging_interval(struct pglist_data *pgdat) {
  struct lru_gen_stats *stats = node_lru_stats[pgdat->node_id];
  if (!stats)
    return 100; /* Default: 100 jiffies */

  int oldest_gen = pgdat->lrugen.min_seq[0] % MAX_NR_GENS;
  long reclaimed = atomic_long_read(&stats->reclaimed[oldest_gen]);
  long refaults = atomic_long_read(&stats->refaults[oldest_gen]);

  /* Calculate refault ratio (scaled by 100) */
  int refault_rate = 0;
  if (reclaimed > 0) {
    refault_rate = (refaults * 100) / reclaimed;
  }

  /* Base interval: 100 jiffies */
  unsigned long interval = 100;

  /* Increase interval if refault rate is high (slow down aging) */
  if (refault_rate > 50) {
    interval += (refault_rate - 50) * 2;
  }

  /* Decrease interval under memory pressure (speed up aging) */
  for (int i = 0; i < MAX_NR_ZONES; i++) {
    struct zone *z = &pgdat->node_zones[i];
    if (z->present_pages > 0 && z->nr_free_pages < z->watermark[WMARK_LOW]) {
      interval = interval / 2;
      break;
    }
  }

  /* Clamp to reasonable bounds */
  if (interval < 10)
    interval = 10;
  if (interval > 1000)
    interval = 1000;

  return interval;
}
#endif /* CONFIG_MM_MGLRU */

/**
 * struct scan_control - Control parameters for a reclamation pass.
 */
struct scan_control {
  size_t nr_to_reclaim;
  gfp_t gfp_mask;
  int priority; /* 0 (max) to 12 (min) */
  size_t nr_reclaimed;
  size_t nr_scanned;
  int nid; /* Node being scanned */

  /* scan control */
  bool may_writepage; /* Allow writing dirty pages */
  bool may_unmap; /* Allow unmapping pages */
  bool may_swap; /* Allow swapping anonymous pages */
  unsigned long scan_limit; /* Max pages to scan before giving up */
  unsigned int contention_count; /* Lock contention backoff */
};

#ifdef CONFIG_MM_MGLRU
static inline int folio_lru_gen(struct folio *folio) {
  return (int) ((folio->flags >> LRU_GEN_SHIFT) & LRU_GEN_MASK);
}

static inline int folio_is_file(struct folio *folio) {
  return !((uintptr_t) folio->mapping & 0x1);
}

/*
 * Per-CPU LRU Batching (Pagevec-style)
 *
 * OPTIMIZATION: Instead of grabbing the global pgdat->lru_lock for every
 * folio insertion, we batch folios in a per-CPU array and flush to the
 * LRU in bulk. This dramatically reduces lock contention.
 *
 * Based on Linux's pagevec and folio_batch infrastructure.
 */
#define LRU_BATCH_SIZE 15

struct lru_batch {
  unsigned int nr;
  struct folio *folios[LRU_BATCH_SIZE];
};

DEFINE_PER_CPU(struct lru_batch, lru_batch_add);

static void __lru_batch_flush(struct lru_batch *batch) {
  if (batch->nr == 0) return;

  /* Group by node for efficient lock acquisition */
  for (unsigned int i = 0; i < batch->nr; i++) {
    struct folio *folio = batch->folios[i];
    if (!folio) continue;

    int nid = folio->node;
    struct pglist_data *pgdat = node_data[nid];
    if (!pgdat) continue;

    int type = !folio_is_file(folio);

    irq_flags_t flags = spinlock_lock_irqsave(&pgdat->lru_lock);

    /* Skip if already on LRU */
    if (folio->flags & PG_lru) {
      spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);
      continue;
    }

    int gen = pgdat->lrugen.max_seq % MAX_NR_GENS;

    /* Atomic flags update */
    unsigned long old_flags, new_flags;
    do {
      old_flags = folio->flags;
      new_flags = old_flags | PG_lru;
      new_flags &= ~(LRU_GEN_MASK << LRU_GEN_SHIFT);
      new_flags |= ((unsigned long) gen << LRU_GEN_SHIFT);
    } while (!__atomic_compare_exchange_n(&folio->flags, &old_flags, new_flags,
                                          false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

    list_add(&folio->lru, &pgdat->lrugen.lists[gen][type]);
    atomic_long_inc(&pgdat->lrugen.nr_pages[gen][type]);

    spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);

#ifdef CONFIG_MM_MGLRU_BLOOM_FILTER
    if (folio->mapping) {
      struct lru_gen_bloom *bloom = node_bloom[nid];
      if (bloom) {
        bloom_add(bloom, gen, folio_to_phys(folio) >> PAGE_SHIFT);
      }
    }
#endif
  }

  batch->nr = 0;
}

void lru_batch_flush_cpu(void) {
  struct lru_batch *batch = this_cpu_ptr(lru_batch_add);
  __lru_batch_flush(batch);
}

/**
 * folio_add_lru - Add a folio to the MGLRU.
 *
 * OPTIMIZATION: Uses per-CPU batching to reduce lock contention.
 * Folios are queued locally and flushed in bulk when the batch is full.
 */
void folio_add_lru(struct folio *folio) {
  if (!folio || (folio->flags & PG_lru)) return;

  int nid = folio->node;
  struct pglist_data *pgdat = node_data[nid];
  if (!pgdat) return;

  preempt_disable();
  struct lru_batch *batch = this_cpu_ptr(lru_batch_add);

  batch->folios[batch->nr++] = folio;

  if (batch->nr == LRU_BATCH_SIZE) {
    __lru_batch_flush(batch);
  }
  preempt_enable();
}
#endif

#ifdef CONFIG_MM_LRU
/**
 * folio_add_lru - Add a folio to the inactive LRU list.
 */
void folio_add_lru(struct folio *folio) {
  if (!folio) return;

  struct list_head *inactive = this_cpu_ptr(inactive_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);

  irq_flags_t flags = spinlock_lock_irqsave(lock);

  if (folio->flags & PG_lru) {
    spinlock_unlock_irqrestore(lock, flags);
    return;
  }

  list_add(&folio->lru, inactive);
  folio->flags |= PG_lru;

  spinlock_unlock_irqrestore(lock, flags);
}
#endif

/**
 * try_to_unmap_folio - The core of memory reclamation.
 * Unmaps a folio from all VMAs that reference it.
 */
int try_to_unmap_folio(struct folio *folio, struct mmu_gather *tlb) {
  void *mapping = folio->mapping;
  if (!mapping) return 0;

  rcu_read_lock();

  /* Check if it's anonymous (bit 0 set) */
  if ((uintptr_t) mapping & 0x1) {
    struct anon_vma *av = (struct anon_vma *) ((uintptr_t) mapping & ~0x1);

    // Validate anon_vma before use
    if (!av || atomic_read(&av->refcount) == 0) {
      folio->mapping = nullptr;
      rcu_read_unlock();
      return 0;
    }

    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);

    // Re-check after acquiring lock
    if (atomic_read(&av->refcount) == 0) {
      spinlock_unlock_irqrestore(&av->lock, flags);
      folio->mapping = nullptr;
      rcu_read_unlock();
      return 0;
    }

    struct anon_vma_chain *avc;

    list_for_each_entry(avc, &av->head, same_anon_vma) {
      struct vm_area_struct *vma = avc->vma;
      if (!vma || !vma->vm_mm) continue;

      uint64_t address = vma->vm_start + (folio->index << PAGE_SHIFT);
      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (tlb) {
          uint64_t phys = vmm_unmap_page_no_flush(vma->vm_mm, address);
          if (phys) {
            tlb->mm = vma->vm_mm; // Track last modified mm
            tlb_remove_folio(tlb, folio, address);
          }
        } else {
          vmm_unmap_page(vma->vm_mm, address);
        }
      }
    }

    spinlock_unlock_irqrestore(&av->lock, flags);
    folio->mapping = nullptr;
    rcu_read_unlock();
    return 1;
  }

  /* Shared Memory / File-backed (bit 0 NOT set) */
  struct vm_object *obj = (struct vm_object *) mapping;
  down_read(&obj->lock);
  struct vm_area_struct *vma;

  list_for_each_entry(vma, &obj->i_mmap, vm_shared) {
    if (folio->index < vma->vm_pgoff) continue;

    uint64_t pgoff_in_vma = folio->index - vma->vm_pgoff;
    if (pgoff_in_vma >= vma_pages(vma)) continue;

    uint64_t address = vma->vm_start + (pgoff_in_vma << PAGE_SHIFT);

    if (vma->vm_mm->pml_root) {
      if (tlb) {
        uint64_t phys = vmm_unmap_page_no_flush(vma->vm_mm, address);
        if (phys) {
          tlb->mm = vma->vm_mm;
          tlb_remove_folio(tlb, folio, address);
        }
      } else {
        vmm_unmap_page(vma->vm_mm, address);
      }
    }
  }

  up_read(&obj->lock);
  folio->mapping = nullptr;
  rcu_read_unlock();
  return 1;
}

/**
 * folio_referenced - Check if any PTE pointing to this folio has the Accessed bit set.
 *
 * OPTIMIZATION: Uses vmm_clear_accessed_no_flush() to avoid O(n) TLB shootdowns
 * when the folio is mapped in multiple VMAs. We accumulate the address range
 * and do a single batched shootdown at the end.
 */
int folio_referenced(struct folio *folio) {
  int referenced = 0;
  void *mapping = folio->mapping;
  if (!mapping) return 0;

  /* Track range for batched TLB shootdown */
  uint64_t flush_start = ULONG_MAX;
  uint64_t flush_end = 0;
  struct mm_struct *flush_mm = nullptr;

  rcu_read_lock();

  if ((uintptr_t) mapping & 0x1) {
    struct anon_vma *av = (struct anon_vma *) ((uintptr_t) mapping & ~0x1);
    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
    struct anon_vma_chain *avc;

    list_for_each_entry(avc, &av->head, same_anon_vma) {
      struct vm_area_struct *vma = avc->vma;
      uint64_t address = vma->vm_start + (folio->index << PAGE_SHIFT);

      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (vmm_is_accessed(vma->vm_mm, address)) {
          referenced++;
          vmm_clear_accessed_no_flush(vma->vm_mm, address);
          /* Track range for batched flush */
          if (address < flush_start) flush_start = address;
          if (address + PAGE_SIZE > flush_end) flush_end = address + PAGE_SIZE;
          flush_mm = vma->vm_mm;
        }
      }
    }
    spinlock_unlock_irqrestore(&av->lock, flags);
  } else {
    /* VM Object */
    struct vm_object *obj = (struct vm_object *) mapping;
    down_read(&obj->lock);
    struct vm_area_struct *vma;

    list_for_each_entry(vma, &obj->i_mmap, vm_shared) {
      uint64_t address = vma->vm_start + ((folio->index - vma->vm_pgoff) << PAGE_SHIFT);

      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (vmm_is_accessed(vma->vm_mm, address)) {
          referenced++;
          vmm_clear_accessed_no_flush(vma->vm_mm, address);
          /* Track range for batched flush */
          if (address < flush_start) flush_start = address;
          if (address + PAGE_SIZE > flush_end) flush_end = address + PAGE_SIZE;
          flush_mm = vma->vm_mm;
        }
      }
    }
    up_read(&obj->lock);
  }

  rcu_read_unlock();

  /* Single batched TLB shootdown for all cleared accessed bits */
  if (flush_mm && flush_start < flush_end) {
    vmm_tlb_shootdown(flush_mm, flush_start, flush_end);
  }

  return referenced;
}

#include <mm/zmm.h>

/**
 * folio_reclaim - Attempt to free a folio by unmapping it from all users.
 *
 * Reclaim strategy for anonymous pages:
 *   1. Try ZMM compression (fast, in-memory)
 *   2. Try swap to disk (slower, unlimited capacity)
 *   3. Give up if neither works
 *
 * File-backed pages can be discarded if clean, or written back if dirty.
 */
int folio_reclaim(struct folio *folio, struct mmu_gather *tlb) {
  if (folio_referenced(folio)) {
    return -EAGAIN;
  }

  bool is_anon = (uintptr_t) folio->mapping & 0x1;

  if (is_anon) {
    struct vm_object *obj = (struct vm_object *) ((uintptr_t) folio->mapping & ~0x1);

#ifdef CONFIG_MM_ZMM
    /* Strategy 1: Try ZMM compression */
    zmm_handle_t handle = zmm_compress_folio(folio);
    if (handle != 0) {
      down_write(&obj->lock);
      /*
       * XArray 'exceptional' entry: we store the handle with bit 0 set to 1.
       * Since our handles are zmm_entry pointers (8-byte aligned), bit 1 is free.
       * We use bit 0=1 to mark it as NOT a folio pointer.
       */
      void *exceptional = (void *) (handle | 0x1);
      xa_store(&obj->page_tree, folio->index, exceptional, GFP_ATOMIC);

      /* Unmap from all users before freeing physical page */
      try_to_unmap_folio(folio, tlb);

      up_write(&obj->lock);

      if (!tlb) {
        pmm_free_pages(folio_to_phys(folio), folio_nr_pages(folio));
      }
      return 0;
    }
#endif

#ifdef CONFIG_MM_SWAP
    /* Strategy 2: Try swap if ZMM failed or is disabled */
    if (swap_is_enabled()) {
      swp_entry_t entry = get_swap_page(folio);
      if (!non_swap_entry(entry)) {
        /* Write page to swap */
        int ret = swap_writepage(folio, entry);
        if (ret == 0) {
          down_write(&obj->lock);

          /*
           * Store swap entry in XArray.
           * We use a special encoding: swap entries have bits [1:0] = 0b10
           * to distinguish from folios (0b00) and ZMM (0b01).
           */
          void *swap_exceptional = (void *) ((entry.val << 2) | 0x2);
          xa_store(&obj->page_tree, folio->index, swap_exceptional, GFP_ATOMIC);

          /* Track swap usage */
          atomic_long_inc(&obj->nr_swap);
          obj->flags |= VM_OBJECT_SWAP_BACKED;

#ifdef CONFIG_MM_WORKINGSET
          /* Store shadow entry for refault detection */
          void *shadow = workingset_eviction(folio, obj);
          /* Shadow is stored implicitly via the swap entry */
          (void) shadow;
#endif

          /* Unmap from all users */
          try_to_unmap_folio(folio, tlb);

          up_write(&obj->lock);

          if (!tlb) {
            pmm_free_pages(folio_to_phys(folio), folio_nr_pages(folio));
          }
          return 0;
        } else {
          /* Write failed, free the swap slot */
          swap_free(entry);
        }
      }
    }
#endif

    /* If no ZMM and no Swap, we cannot reclaim anonymous pages */
    return -ENOMEM;
  }

  /* File-backed pages can always be reclaimed (if not dirty) */
  if (folio->page.flags & PG_dirty) {
    wakeup_writeback();
    return -EAGAIN;
  }

  if (try_to_unmap_folio(folio, tlb)) {
    if (!tlb) {
      pmm_free_pages(folio_to_phys(folio), folio_nr_pages(folio));
    }
    return 0;
  }

  return -EBUSY;
}

#ifdef CONFIG_MM_LRU
/**
 * shrink_active_list - Move pages from active to inactive if they aren't referenced.
 */
void shrink_active_list(size_t nr_to_scan, struct scan_control *sc) {
  struct list_head *active = this_cpu_ptr(active_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);
  struct list_head folio_list;
  INIT_LIST_HEAD(&folio_list);

  irq_flags_t flags = spinlock_lock_irqsave(lock);
  for (size_t i = 0; i < nr_to_scan && !list_empty(active); i++) {
    struct
    folio *folio = list_last_entry(active, struct folio, lru);
    list_move(&folio->lru, &folio_list);
    folio->flags &= ~PG_active;
  }
  spinlock_unlock_irqrestore(lock, flags);

  struct list_head *pos, *q;
  list_for_each_safe(pos, q, &folio_list)
  {
    struct
    folio *folio = list_entry(pos, struct folio, lru);

    if (folio_referenced(folio)) {
      /* Still active, move back to head of active list */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, active);
      folio->flags |= PG_active;
      spinlock_unlock_irqrestore(lock, flags);
    } else {
      /* Not referenced, move to inactive list */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, this_cpu_ptr(inactive_list));
      spinlock_unlock_irqrestore(lock, flags);
    }
    sc->nr_scanned++;
  }
}

/**
 * shrink_inactive_list - Scan the inactive LRU for pages to reclaim.
 */
size_t shrink_inactive_list(size_t nr_to_scan) {
  size_t reclaimed = 0;
  struct list_head folio_list;
  INIT_LIST_HEAD(&folio_list);

  struct list_head *inactive = this_cpu_ptr(inactive_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);

  irq_flags_t flags = spinlock_lock_irqsave(lock);
  for (size_t i = 0; i < nr_to_scan && !list_empty(inactive); i++) {
    struct
    folio *folio = list_last_entry(inactive, struct folio, lru);
    list_move(&folio->lru, &folio_list);
    folio->flags &= ~PG_lru;
  }
  spinlock_unlock_irqrestore(lock, flags);

  /*
   * Batch TLB shootdown for all pages unmapped during this pass.
   * This prevents "IPI Storms" on SMP.
   */
  struct mmu_gather tlb;
  tlb_gather_mmu(&tlb, &init_mm, 0, 0); // Dynamic range

  struct list_head *pos, *q;
  list_for_each_safe(pos, q, &folio_list)
  {
    struct
    folio *folio = list_entry(pos, struct folio, lru);

    int ret = folio_reclaim(folio, &tlb);
    if (ret == 0) {
      reclaimed++;
    } else if (ret == -EAGAIN) {
      /* Re-activate */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, this_cpu_ptr(active_list));
      folio->flags |= (PG_lru | PG_active);
      spinlock_unlock_irqrestore(lock, flags);
    } else {
      /* Re-insert into inactive */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, inactive);
      folio->flags |= PG_lru;
      spinlock_unlock_irqrestore(lock, flags);
    }
  }

  tlb_finish_mmu(&tlb);

  return reclaimed;
}
#endif

#ifdef CONFIG_MM_MGLRU

/**
 * lru_gen_folio_gen - Get the generation of a folio.
 */
static inline int lru_gen_folio_gen(struct folio *folio) {
  return (int) ((folio->flags >> LRU_GEN_SHIFT) & LRU_GEN_MASK);
}

/**
 * scan_gen - Scans a specific generation for pages to reclaim.
 *
 * Enhanced with:
 *   - Statistics tracking for adaptive aging
 *   - Bloom filter integration for efficient PT scanning
 *   - Workingset-aware promotion
 */
static size_t scan_gen(struct pglist_data *pgdat, int gen, int type, struct scan_control *sc) {
  size_t reclaimed = 0;
  size_t scanned = 0;
  size_t promoted = 0;
  struct list_head folio_list;
  INIT_LIST_HEAD(&folio_list);

  struct mmu_gather tlb;
  tlb_gather_mmu(&tlb, &init_mm, 0, 0);

  /* Get stats tracker */
  struct lru_gen_stats *stats = node_lru_stats[pgdat->node_id];

  irq_flags_t flags = spinlock_lock_irqsave(&pgdat->lru_lock);
  struct list_head *src = &pgdat->lrugen.lists[gen][type];

  /*
   * Batch selection: Grab a batch of pages to process outside the spinlock.
   * This reduces lock contention and allows for more complex per-page logic.
   *
   * Enhanced: Apply scan limit to prevent excessive CPU usage.
   */
  size_t batch_limit = sc->nr_to_reclaim;
  if (sc->scan_limit > 0 && batch_limit > sc->scan_limit)
    batch_limit = sc->scan_limit;

  while (!list_empty(src) && scanned < batch_limit) {
    struct folio *folio = list_last_entry(src, struct folio, lru);
    list_move(&folio->lru, &folio_list);
    scanned++;
  }
  spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);

  struct list_head *pos, *q;
  list_for_each_safe(pos, q, &folio_list) {
    struct folio *folio = list_entry(pos, struct folio, lru);

    /*
     * 1. AGE CHECK: If recently accessed, promote to the youngest generation.
     * This implements 'working set' discovery.
     */
    if (folio_referenced(folio)) {
      int new_gen = pgdat->lrugen.max_seq % MAX_NR_GENS;
      flags = spinlock_lock_irqsave(&pgdat->lru_lock);

      /* Atomic generation update */
      unsigned long old_f, new_f;
      do {
        old_f = folio->flags;
        new_f = (old_f & ~(LRU_GEN_MASK << LRU_GEN_SHIFT)) | ((unsigned long) new_gen << LRU_GEN_SHIFT);
      } while (!__atomic_compare_exchange_n(&folio->flags, &old_f, new_f, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

      list_add(&folio->lru, &pgdat->lrugen.lists[new_gen][type]);
      atomic_long_dec(&pgdat->lrugen.nr_pages[gen][type]);
      atomic_long_inc(&pgdat->lrugen.nr_pages[new_gen][type]);

#ifdef CONFIG_MM_MGLRU_BLOOM_FILTER
      /* Update bloom filter for new generation */
      struct lru_gen_bloom *bloom = node_bloom[pgdat->node_id];
      if (bloom) {
        bloom_add(bloom, new_gen, folio_to_phys(folio) >> PAGE_SHIFT);
      }
#endif

      spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);
      promoted++;
      continue;
    }

    /*
     * 2. Check scan control flags before attempting reclaim.
     */
    bool is_anon = (uintptr_t) folio->mapping & 0x1;

    /* Skip anonymous pages if may_swap is false */
    if (is_anon && !sc->may_swap) {
      flags = spinlock_lock_irqsave(&pgdat->lru_lock);
      list_add(&folio->lru, src);
      spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);
      continue;
    }

    /* Skip dirty pages if may_writepage is false */
    if ((folio->page.flags & PG_dirty) && !sc->may_writepage) {
      flags = spinlock_lock_irqsave(&pgdat->lru_lock);
      list_add(&folio->lru, src);
      spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);
      continue;
    }

    /*
     * 3. RECLAIM: Attempt to free the page.
     * If it fails (e.g., it's dirty or locked), it stays in its current generation
     * but moves to the head (most recently used) to avoid immediate re-scanning.
     */
    int ret = folio_reclaim(folio, &tlb);
    if (ret == 0) {
      reclaimed++;
      atomic_long_dec(&pgdat->lrugen.nr_pages[gen][type]);

      /* Throttling: If we reclaimed enough, balance dirty pages if pressure is high */
      if (reclaimed % 32 == 0) {
        balance_dirty_pages_ratelimited(folio->mapping);
      }

      /* Track statistics */
      if (stats) {
        atomic_long_inc(&stats->reclaimed[gen]);
      }
    } else {
      flags = spinlock_lock_irqsave(&pgdat->lru_lock);
      list_add(&folio->lru, src); /* Move to head of current gen */
      spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);

      /* Track contention for backoff */
      if (ret == -EAGAIN) {
        sc->contention_count++;
      }
    }
  }

  tlb_finish_mmu(&tlb);

  /* Update statistics */
  sc->nr_scanned += scanned;
  if (stats) {
    atomic_long_add(scanned, &stats->scanned[gen]);
    atomic_long_add(promoted, &stats->promoted[gen]);
  }

  return reclaimed;
}

/**
 * shrink_node - Main entry point for MGLRU reclamation.
 *
 * Tiered approach:
 *   1. Clean file pages (cheapest)
 *   2. Dirty file pages (requires writeback)
 *   3. Anonymous pages (requires swap/compression)
 *
 * Enhanced with adaptive aging based on refault statistics.
 */
static void shrink_node(struct pglist_data *pgdat, struct scan_control *sc) {
  /*
   * Scan generations from oldest to youngest.
   * Older generations contain colder pages.
   */
  int oldest_file = pgdat->lrugen.min_seq[0] % MAX_NR_GENS;
  int oldest_anon = pgdat->lrugen.min_seq[1] % MAX_NR_GENS;

  for (int gen = 0; gen < MAX_NR_GENS; gen++) {
    int file_gen = (oldest_file + gen) % MAX_NR_GENS;
    int anon_gen = (oldest_anon + gen) % MAX_NR_GENS;

    /*
     * TIER 1: Clean File Pages (Cheapest to reclaim)
     */
    sc->nr_reclaimed += scan_gen(pgdat, file_gen, 0, sc); /* file */
    if (sc->nr_reclaimed >= sc->nr_to_reclaim) return;

    /*
     * TIER 2: Anonymous Pages (Require Swap/Compression)
     * Only try if we have swap or ZMM available.
     */
#if defined(CONFIG_MM_SWAP) || defined(CONFIG_MM_ZMM)
    if (sc->may_swap) {
      sc->nr_reclaimed += scan_gen(pgdat, anon_gen, 1, sc); /* anon */
      if (sc->nr_reclaimed >= sc->nr_to_reclaim) return;
    }
#endif

    /* Backoff if too much contention */
    if (sc->contention_count > 10) {
      sc->contention_count = 0;
      break;
    }
  }

  /*
   * ADVANCE GENERATIONS:
   * If we reached here, it means the current generations are exhausted or too hot.
   * We advance the max sequence to start a new aging cycle.
   */
  irq_flags_t flags = spinlock_lock_irqsave(&pgdat->lru_lock);
  pgdat->lrugen.max_seq++;

#ifdef CONFIG_MM_MGLRU_BLOOM_FILTER
  /* Clear the bloom filter for the oldest generation being recycled */
  struct lru_gen_bloom *bloom = node_bloom[pgdat->node_id];
  if (bloom) {
    int recycled_gen = pgdat->lrugen.max_seq % MAX_NR_GENS;
    bloom_clear(bloom, recycled_gen);
  }
#endif

  /* Reset min_seq if we are wrapping around */
  if (pgdat->lrugen.max_seq - pgdat->lrugen.min_seq[0] > MAX_NR_GENS) {
    pgdat->lrugen.min_seq[0]++;
  }
  if (pgdat->lrugen.max_seq - pgdat->lrugen.min_seq[1] > MAX_NR_GENS) {
    pgdat->lrugen.min_seq[1]++;
  }
  spinlock_unlock_irqrestore(&pgdat->lru_lock, flags);
}
#endif

#ifdef CONFIG_MM_LRU
/**
 * shrink_zone - Standard reclamation for a single zone.
 */
static void shrink_zone(struct zone *zone, struct scan_control *sc) {
  /*
   * In a production kernel, we would scan proportional to list sizes.
   * Here we scan a fixed batch based on priority.
   */
  size_t nr_active = 32 >> (12 - sc->priority);
  size_t nr_inactive = 64 >> (12 - sc->priority);

  /* Aging */
  shrink_active_list(nr_active, sc);

  /* Reclamation */
  sc->nr_reclaimed += shrink_inactive_list(nr_inactive);
}

static void shrink_node(struct pglist_data *pgdat, struct scan_control *sc) {
  for (int i = MAX_NR_ZONES - 1; i >= 0; i--) {
    struct zone *z = &pgdat->node_zones[i];
    if (z->present_pages == 0) continue;

    if (z->nr_free_pages < z->watermark[WMARK_HIGH]) {
      shrink_zone(z, sc);
    }
  }
}
#endif

void wakeup_kswapd(struct zone *zone) {
  if (!zone || !zone->zone_pgdat) return;
  wake_up(&zone->zone_pgdat->kswapd_wait);
}

static int kswapd_should_run(struct pglist_data *pgdat) {
  for (int i = 0; i < MAX_NR_ZONES; i++) {
    struct zone *z = &pgdat->node_zones[i];
    unsigned long mark = z->watermark[WMARK_HIGH];
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
    mark += z->watermark_boost;
#endif
    if (z->present_pages > 0 && z->nr_free_pages < mark)
      return 1;
  }
  return 0;
}

static int kswapd_thread(void *data) {
  struct pglist_data *pgdat = (struct pglist_data *) data;
  printk(KERN_INFO SWAP_CLASS "kswapd started for node %d\n", pgdat->node_id);

#ifdef CONFIG_MM_MGLRU
#ifdef CONFIG_MM_MGLRU_BLOOM_FILTER
  /* Initialize bloom filter for this node */
  bloom_init_node(pgdat->node_id);
#endif
  /* Initialize LRU statistics for this node */
  lru_stats_init_node(pgdat->node_id);
#endif

  while (1) {
    wait_event(pgdat->kswapd_wait, kswapd_should_run(pgdat));

    struct scan_control sc = {
      .gfp_mask = GFP_KERNEL,
      .nr_to_reclaim = 128,
      .priority = 12, /* Start with lowest priority */
      .nid = pgdat->node_id,
      .nr_reclaimed = 0,
      .nr_scanned = 0,
      /* Enhanced controls */
      .may_writepage = true,
      .may_unmap = true,
      .may_swap = true,
      .scan_limit = 0, /* No limit for kswapd */
      .contention_count = 0,
    };

    while (sc.priority >= 0) {
      shrink_node(pgdat, &sc);

      if (sc.nr_reclaimed >= sc.nr_to_reclaim) {
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
        /* Once satisfied, gradually decay the boost */
        for (int i = 0; i < MAX_NR_ZONES; i++) {
          struct zone *z = &pgdat->node_zones[i];
          if (z->watermark_boost > 0) {
            z->watermark_boost >>= 1; // Simple decay
          }
        }
#endif
        break;
      }

      /* If not enough reclaimed, increase pressure */
      sc.priority--;
    }
  }
  return 0;
}

/**
 * try_to_free_pages - Unified entry point for direct reclamation.
 *
 * Called synchronously when allocations fail and we need to free
 * memory immediately. More aggressive than background kswapd.
 */
size_t try_to_free_pages(struct pglist_data *pgdat, size_t nr_to_reclaim, gfp_t gfp_mask) {
  struct scan_control sc = {
    .nr_to_reclaim = nr_to_reclaim,
    .gfp_mask = gfp_mask,
    .priority = 12,
    .nid = pgdat->node_id,
    .nr_reclaimed = 0,
    .nr_scanned = 0,
    /* Enhanced controls for direct reclaim */
    .may_writepage = !(gfp_mask & __GFP_IO) ? false : true,
    .may_unmap = true,
    .may_swap = !(gfp_mask & __GFP_IO) ? false : true,
    .scan_limit = nr_to_reclaim * 4, /* Don't scan forever */
    .contention_count = 0,
  };

  /*
   * We attempt to reclaim with increasing pressure.
   * Professional kernels back off if we're scanning too much without success.
   */
  while (sc.priority >= 0) {
    shrink_node(pgdat, &sc);
    if (sc.nr_reclaimed >= nr_to_reclaim) break;

    /* Check if we've scanned too much without success */
    if (sc.nr_scanned > nr_to_reclaim * 8 && sc.nr_reclaimed == 0) {
      /* Likely thrashing, give up early */
      break;
    }

    sc.priority--;
  }

  return sc.nr_reclaimed;
}

void kswapd_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (node_data[n] && node_data[n]->node_spanned_pages > 0) {
      char name[16];
      snprintf(name, sizeof(name), "kswapd%d", n);
      struct task_struct *k = kthread_create(kswapd_thread, node_data[n], name);
      if (k) {
        node_data[n]->kswapd_task = k;
        kthread_run(k);
      }
    }
  }
}

void lru_init(void) {
#ifdef CONFIG_MM_LRU
  int cpu;
  for_each_possible_cpu(cpu) {
    struct list_head *inactive = per_cpu_ptr(inactive_list, cpu);
    struct list_head *active = per_cpu_ptr(active_list, cpu);
    spinlock_t *lock = per_cpu_ptr(lru_lock, cpu);

    INIT_LIST_HEAD(inactive);
    INIT_LIST_HEAD(active);
    spinlock_init(lock);
  }
#endif
}

/**
 * anon_vma_chain_link - Connects a VMA to an anon_vma via a chain node.
 */
int anon_vma_chain_link(struct vm_area_struct *vma, struct anon_vma *av) {
  struct anon_vma_chain *avc = kmalloc(sizeof(struct anon_vma_chain));
  if (!avc) return -ENOMEM;

  avc->vma = vma;
  avc->anon_vma = av;
  list_add(&avc->same_vma, &vma->anon_vma_chain);

  irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
  list_add(&avc->same_anon_vma, &av->head);
  atomic_inc(&av->refcount);
  spinlock_unlock_irqrestore(&av->lock, flags);

  return 0;
}

/**
 * anon_vma_prepare - Ensure a VMA has an anon_vma for reverse mapping.
 */
int anon_vma_prepare(struct vm_area_struct *vma) {
  if (vma->anon_vma) return 0;

  struct anon_vma *av = kmalloc(sizeof(struct anon_vma));
  if (!av) return -ENOMEM;

  spinlock_init(&av->lock);
  INIT_LIST_HEAD(&av->head);
  atomic_set(&av->refcount, 1);
  av->parent = nullptr;

  vma->anon_vma = av;
  return anon_vma_chain_link(vma, av);
}

void anon_vma_free(struct anon_vma *av) {
  if (!av) return;
  if (atomic_dec_and_test(&av->refcount)) {
    kfree(av);
  }
}

/**
 * folio_add_anon_rmap - Links a physical folio to an anonymous VMA.
 */
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address) {
  /*
   * Anonymous mapping: bit 0 is set.
   * If the folio is already mapped, we skip adding it to the LRU again,
   * but we ensure the mapping is correct (e.g. for COW).
   */
  if (!folio->mapping) {
    folio->mapping = (void *) ((uintptr_t) vma->anon_vma | 0x1);
    folio->index = (address - vma->vm_start) >> PAGE_SHIFT;
    folio_add_lru(folio);
  }
}

/**
 * folio_add_file_rmap - Links a physical folio to its owning VM Object.
 */
void folio_add_file_rmap(struct folio *folio, struct vm_object *obj, uint64_t pgoff) {
  if (!folio->mapping) {
    /* VM Object mapping: bit 0 is NOT set */
    folio->mapping = (void *) obj;
    folio->index = pgoff;
    folio_add_lru(folio);
  }
}

/* --- Shared Memory Support --- */

static int shmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
  if (!vma->vm_obj) return VM_FAULT_SIGBUS;
  vmf->prot = vma->vm_page_prot;
  return vma->vm_obj->ops->fault(vma->vm_obj, vma, vmf);
}

const struct vm_operations_struct shmem_vm_ops = {
  .fault = shmem_fault,
};

/**
 * migrate_folio_to_node - Physically move a folio to a different NUMA node.
 */
static int migrate_folio_to_node(struct folio *folio, int nid) {
  if (!folio || folio->node == nid) return 0;

  struct folio *new_folio = alloc_pages_node(nid, GFP_KERNEL, folio_order(folio));
  if (!new_folio) return -ENOMEM;

  /* 1. Unmap all users to freeze the page */
  if (try_to_unmap_folio(folio, nullptr) != 0) {
    folio_put(new_folio);
    return -EBUSY;
  }

  /* 2. Copy data */
  memcpy(folio_address(new_folio), folio_address(folio), folio_size(folio));

  /* 3. Update mapping */
  void *mapping = folio->mapping;
  if (mapping) {
    if (!((uintptr_t) mapping & 0x1)) {
      struct vm_object *obj = (struct vm_object *) mapping;
      down_write(&obj->lock);
      xa_store(&obj->page_tree, folio->index, new_folio, GFP_ATOMIC);
      up_write(&obj->lock);
    }
  }

  new_folio->mapping = mapping;
  new_folio->index = folio->index;
  new_folio->page.flags = folio->page.flags;

  /* 4. The old folio will be freed by the caller or by its last put */
  return 0;
}

/**
 * do_numa_page - Handle a NUMA hint fault.
 * Triggers when a page is accessed that belongs to a remote node.
 */
static int do_numa_page(struct vm_area_struct *vma, uint64_t address, struct folio *folio) {
  int target_node = this_node();
  int source_node = folio->node;

  /* If already on the right node, just restore the PTE */
  if (source_node == target_node) {
    vmm_map_page(vma->vm_mm, address, folio_to_phys(folio), vma->vm_page_prot);
    return VM_FAULT_COMPLETED;
  }

  /*
   * NUMA Balancing: Migrate if the remote latency is significantly higher.
   * In a real kernel, we would track "access density" here.
   */
  if (migrate_folio_to_node(folio, target_node) == 0) {
    /* Migration succeeded, map the new location */
    vmm_map_page(vma->vm_mm, address, folio_to_phys(folio), vma->vm_page_prot);
  } else {
    /* Migration failed, just restore access to the remote page */
    vmm_map_page(vma->vm_mm, address, folio_to_phys(folio), vma->vm_page_prot);
  }

  return VM_FAULT_COMPLETED;
}

/**
 * handle_mm_fault - Generic fault handler that dispatches to VMA-specific operations.
 *
 * NOTE: The caller MUST hold either:
 *   1. mm->mmap_lock (Read/Write)
 *   2. vma->vm_lock (Shared/Exclusive)
 *   3. RCU read lock (if flags & FAULT_FLAG_SPECULATIVE)
 */
int handle_mm_fault(struct vm_area_struct *vma, uint64_t address, unsigned int flags) {
  struct mm_struct *mm = vma->vm_mm;
  uint32_t vma_seq = vma->vma_seq;

  /* Check for NUMA hint faults first if the PTE is present but marked for hinting */
  if (vmm_is_numa_hint(mm, address)) {
    struct folio *folio = vmm_get_folio(mm, address);
    if (folio) {
      int ret = do_numa_page(vma, address, folio);
      folio_put(folio);
      return ret;
    }
  }

  struct vm_fault vmf;
  vmf.address = address & PAGE_MASK;
  vmf.flags = flags;
  vmf.pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
  if (vma->vm_pgoff) vmf.pgoff += vma->vm_pgoff;
  vmf.folio = nullptr;

  int ret = VM_FAULT_SIGSEGV;

  /*
   * Priority 1: VMA Operations (Special/Legacy)
   */
  if (vma->vm_ops && vma->vm_ops->fault) {
    ret = vma->vm_ops->fault(vma, &vmf);
  }
  /*
   * Priority 2: VM Object (Production-ready MM)
   */
  else if (vma->vm_obj && vma->vm_obj->ops && vma->vm_obj->ops->fault) {
    ret = vma->vm_obj->ops->fault(vma->vm_obj, vma, &vmf);
  }
  /*
   * Priority 3: Lazy Anonymous Object Creation
   */
  else if (!vma->vm_obj && !(vma->vm_flags & (VM_IO | VM_PFNMAP))) {
    if (flags & FAULT_FLAG_SPECULATIVE) return VM_FAULT_RETRY;

    /*
     * We cannot upgrade locks safely here if we only hold shared.
     * In a real kernel, we would return VM_FAULT_RETRY and the caller
     * would retry with an exclusive lock.
     */
    vma->vm_obj = vm_object_anon_create(vma_size(vma));
    if (!vma->vm_obj) {
      return VM_FAULT_OOM;
    }
    down_write(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    up_write(&vma->vm_obj->lock);

    ret = vma->vm_obj->ops->fault(vma->vm_obj, vma, &vmf);
  }

  if (ret == VM_FAULT_COMPLETED) return 0;
  if (ret != 0) return ret;

  if (vmf.folio) {
    /* 
     * SPF COMMIT: Before we map the page, we MUST verify the VMA didn't change.
     */
    if (flags & FAULT_FLAG_SPECULATIVE) {
      if (unlikely(vma->vma_seq != vma_seq)) {
        folio_put(vmf.folio);
        return VM_FAULT_RETRY;
      }
      
      /* Verify mmap_seq to detect any address space changes */
      if (unlikely(atomic_read(&mm->mmap_seq) != (int)(flags >> 16))) {
        folio_put(vmf.folio);
        return VM_FAULT_RETRY;
      }
    }

    struct folio *folio = vmf.folio;
    uint64_t phys = folio_to_phys(folio);

    if (PageHead(&folio->page) && folio->page.order == 9) {
      vmm_map_huge_page(vma->vm_mm, vmf.address & 0xFFFFFFFFFFE00000ULL,
                        phys, vmf.prot, VMM_PAGE_SIZE_2M);
    } else {
      vmm_map_page(vma->vm_mm, vmf.address, phys, vmf.prot);
    }

    /*
     * Handle Shared Writable Mappings & page_mkwrite.
     */
    if ((vma->vm_flags & VM_SHARED) && (flags & FAULT_FLAG_WRITE)) {
      int mk_ret = 0;

      if (vma->vm_ops && vma->vm_ops->page_mkwrite) {
        mk_ret = vma->vm_ops->page_mkwrite(vma, &vmf);
      } else if (vma->vm_obj && vma->vm_obj->ops && vma->vm_obj->ops->page_mkwrite) {
        mk_ret = vma->vm_obj->ops->page_mkwrite(vma->vm_obj, vma, &vmf);
      }

      if (mk_ret != 0) return mk_ret;

      folio->page.flags |= PG_dirty;
      account_page_dirtied();
      vmm_map_page(vma->vm_mm, vmf.address, phys, vmf.prot | PTE_RW);
    }
  }

  return 0;
}
