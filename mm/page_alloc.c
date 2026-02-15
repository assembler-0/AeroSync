/// SPDX-License-Identifier: GPL-2.0-only
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

#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/timer.h>
#include <arch/x86_64/cpu.h>
#include <lib/math.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <mm/gfp.h>
#include <mm/page.h>
#include <mm/zone.h>
#include <aerosync/resdomain.h>
#include <aerosync/export.h>

#define PAGE_POISON_FREE 0xfe
#define PAGE_POISON_ALLOC 0xad

#define ALLOC_HIGHATOMIC 0x01

#ifndef CONFIG_MM_PMM_DEFERRED_BATCH_SIZE
#define DEFERRED_BATCH_SIZE 32
#else
#define DEFERRED_BATCH_SIZE CONFIG_MM_PMM_DEFERRED_BATCH_SIZE
#endif

#ifdef CONFIG_MM_PMM_PAGEBLOCK_METADATA
#define PAGEBLOCK_BITS 4
#define pageblock_nr_pages PAGEBLOCK_NR_PAGES

static inline unsigned long pfn_to_pageblock_nr(unsigned long pfn) {
  return pfn >> PAGEBLOCK_ORDER;
}

static inline int get_pageblock_migratetype(struct zone *zone,
                                            unsigned long pfn) {
  unsigned long pb_nr = pfn_to_pageblock_nr(pfn);
  unsigned long idx = pb_nr / (sizeof(unsigned long) * 8 / PAGEBLOCK_BITS);
  unsigned long shift =
      (pb_nr % (sizeof(unsigned long) * 8 / PAGEBLOCK_BITS)) * PAGEBLOCK_BITS;
  return (zone->pageblock_flags[idx] >> shift) & ((1UL << PAGEBLOCK_BITS) - 1);
}

static inline void set_pageblock_migratetype(struct zone *zone,
                                             unsigned long pfn,
                                             int migratetype) {
  unsigned long pb_nr = pfn_to_pageblock_nr(pfn);
  unsigned long idx = pb_nr / (sizeof(unsigned long) * 8 / PAGEBLOCK_BITS);
  unsigned long shift =
      (pb_nr % (sizeof(unsigned long) * 8 / PAGEBLOCK_BITS)) * PAGEBLOCK_BITS;
  unsigned long mask = ((1UL << PAGEBLOCK_BITS) - 1) << shift;
  zone->pageblock_flags[idx] = (zone->pageblock_flags[idx] & ~mask) |
                               ((unsigned long)migratetype << shift);
}
#endif

#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
static inline void set_free_area_bit(struct zone *zone, unsigned int order,
                                     int migratetype) {
  zone->free_area_bitmap[order] |= (1UL << migratetype);
}

static inline void clear_free_area_bit(struct zone *zone, unsigned int order,
                                       int migratetype) {
  if (list_empty(&zone->free_area[order].free_list[migratetype]))
    zone->free_area_bitmap[order] &= ~(1UL << migratetype);
}

static inline bool test_free_area_bit(struct zone *zone, unsigned int order,
                                      int migratetype) {
  return !!(zone->free_area_bitmap[order] & (1UL << migratetype));
}

static inline bool has_free_area(struct zone *zone, unsigned int order) {
  return zone->free_area_bitmap[order] != 0;
}
#endif

static void kernel_poison_pages(struct page *page, int numpages, uint8_t val) {
#ifdef MM_HARDENING
  void *addr = page_address(page);
  memset(addr, val, (size_t)numpages << PAGE_SHIFT);
  for (int i = 0; i < numpages; i++)
    SetPagePoisoned(&page[i]);
#else
  (void)page;
  (void)numpages;
  (void)val;
#endif
}

static void check_page_poison(struct page *page, int numpages) {
#ifdef MM_HARDENING
  uint64_t *p = (uint64_t *)page_address(page);
  size_t count = (size_t)numpages << (PAGE_SHIFT - 3);
  uint64_t expected = 0xfefefefefefefefeULL;

  /*
   * Optimization: Only check poison if the page was explicitly
   * poisoned. At boot, we skip poisoning to save time. Usable
   * RAM from bootloader is considered safe for the first use.
   */
  if (!PagePoisoned(page))
    return;

  for (size_t i = 0; i < count; i++) {
    if (unlikely(p[i] != expected)) {
      /* Fallback to byte-by-byte to find the exact corrupt byte for the panic
       * message */
      uint8_t *byte_p = (uint8_t *)p;
      size_t byte_size = (size_t)numpages << PAGE_SHIFT;
      for (size_t j = 0; j < byte_size; j++) {
        if (byte_p[j] != PAGE_POISON_FREE) {
          panic(PMM_CLASS "Page poisoning corruption detected at %p (offset "
                          "%zu, val 0x%02x)\n",
                byte_p, j, byte_p[j]);
        }
      }
    }
  }

  for (int i = 0; i < numpages; i++)
    ClearPagePoisoned(&page[i]);
#else
  (void)page;
  (void)numpages;
#endif
}

/* Global zones */
struct zone managed_zones[MAX_NR_ZONES];

/* Default zone names */
static const char *const zone_names[MAX_NR_ZONES] = {"DMA", "DMA32", "Normal"};

/* Defined in arch/x86_64/mm/pmm.c */

extern void wakeup_kswapd(struct zone *zone);

extern size_t try_to_free_pages(struct pglist_data *pgdat, size_t nr_to_reclaim,
                                gfp_t gfp_mask);

/*
 * Debugging helper
 */
static void check_page_sanity(struct page *page, int order) {
  if (PageBuddy(page)) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             PMM_CLASS "Bad page state: PageBuddy set in alloc path (pfn %llu)",
             (uint64_t)(page - mem_map));
    panic(buf);
  }
  if (page->order != 0 && page->order != order) {
    page->order = 0;
  }
}

/*
 * Buddy System Core
 */

static PMM_INLINE unsigned long __find_buddy_pfn(unsigned long page_pfn,
                                                 unsigned int order) {
  return page_pfn ^ (1UL << order);
}

static PMM_INLINE bool page_is_buddy(struct page *page, struct page *buddy,
                                     unsigned int order) {
  if (unlikely(!PageBuddy(buddy)))
    return false;
  if (unlikely(buddy->order != order))
    return false;
  return true;
}

static PMM_INLINE int gfp_to_migratetype(gfp_t gfp_mask) {
#ifdef CONFIG_MM_PMM_HIGHATOMIC
  if (gfp_mask & __GFP_HIGH)
    return MIGRATE_HIGHATOMIC;
#endif
  if (gfp_mask & ___GFP_MOVABLE)
    return MIGRATE_MOVABLE;
  if (gfp_mask & ___GFP_RECLAIMABLE)
    return MIGRATE_RECLAIMABLE;
  return MIGRATE_UNMOVABLE;
}

/*
 * Fallback table for migration types.
 * Defines which migration types can be borrowed from when a specific type is
 * empty.
 */
static int fallbacks[MIGRATE_TYPES][MIGRATE_TYPES] = {
    [MIGRATE_UNMOVABLE] = {MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE,
#ifdef CONFIG_MM_PMM_HIGHATOMIC
                           MIGRATE_HIGHATOMIC,
#endif
                           MIGRATE_TYPES},
    [MIGRATE_RECLAIMABLE] = {MIGRATE_UNMOVABLE, MIGRATE_MOVABLE,
#ifdef CONFIG_MM_PMM_HIGHATOMIC
                             MIGRATE_HIGHATOMIC,
#endif
                             MIGRATE_TYPES},
    [MIGRATE_MOVABLE] = {MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE,
#ifdef CONFIG_MM_PMM_HIGHATOMIC
                         MIGRATE_HIGHATOMIC,
#endif
                         MIGRATE_TYPES},
#ifdef CONFIG_MM_PMM_HIGHATOMIC
    [MIGRATE_HIGHATOMIC] = {MIGRATE_TYPES},
#endif
#ifdef CONFIG_MM_PMM_CMA
    [MIGRATE_CMA] = {MIGRATE_MOVABLE, MIGRATE_TYPES},
#endif
};

static inline void expand(struct zone *zone, struct page *page, int low,
                          int high, struct free_area *area, int migratetype) {
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
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
    set_free_area_bit(zone, high, migratetype);
#endif
    if (high > (int)zone->max_free_order)
      zone->max_free_order = high;
  }
}

static struct page *__rmqueue_fallback(struct zone *zone, unsigned int order,
                                       int start_migratetype) {
  struct free_area *area;
  unsigned int current_order;
  int i;
  int migratetype;

  for (current_order = MAX_ORDER - 1; current_order >= order; current_order--) {
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
    if (!has_free_area(zone, current_order))
      continue;
#endif

    for (i = 0; i < MIGRATE_TYPES; i++) {
      migratetype = fallbacks[start_migratetype][i];
      if (migratetype == MIGRATE_TYPES)
        break;

#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
      if (!test_free_area_bit(zone, current_order, migratetype))
        continue;
#endif

      area = &zone->free_area[current_order];
      if (list_empty(&area->free_list[migratetype]))
        continue;

      struct page *page =
          list_entry(area->free_list[migratetype].next, struct page, list);
      list_del(&page->list);

      ClearPageBuddy(page);
      area->nr_free--;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
      clear_free_area_bit(zone, current_order, migratetype);
#endif
      zone->nr_free_pages -= (1UL << current_order);

#ifdef CONFIG_MM_PMM_PAGEBLOCK_METADATA
      if (current_order >= PAGEBLOCK_ORDER) {
        unsigned long pfn = (unsigned long)(page - mem_map);
        unsigned long start_pfn = pfn & ~(PAGEBLOCK_NR_PAGES - 1);
        unsigned long end_pfn = start_pfn + PAGEBLOCK_NR_PAGES;

        set_pageblock_migratetype(zone, pfn, start_migratetype);

        for (unsigned long move_pfn = start_pfn; move_pfn < end_pfn;
             move_pfn++) {
          struct page *move_page = mem_map + move_pfn;
          if (move_page == page || !PageBuddy(move_page))
            continue;
          if (move_page->migratetype == migratetype) {
            list_del(&move_page->list);
            int move_order = move_page->order;
            zone->free_area[move_order].nr_free--;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
            clear_free_area_bit(zone, move_order, migratetype);
#endif
            move_page->migratetype = start_migratetype;
            list_add(&move_page->list,
                     &zone->free_area[move_order].free_list[start_migratetype]);
            zone->free_area[move_order].nr_free++;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
            set_free_area_bit(zone, move_order, start_migratetype);
#endif
#ifdef CONFIG_MM_PMM_MIGRATION_TRACKING
            atomic_long_inc(&zone->pageblock_steal_count);
#endif
          }
        }
      }
#endif

      expand(zone, page, order, current_order, area, start_migratetype);
      page->migratetype = start_migratetype;
      return page;
    }
  }

  return nullptr;
}

static struct page *__rmqueue(struct zone *zone, unsigned int order,
                              int migratetype) {
  unsigned int current_order;
  struct free_area *area;
  struct page *page;

  for (current_order = order; current_order < MAX_ORDER; ++current_order) {
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
    if (!test_free_area_bit(zone, current_order, migratetype))
      continue;
#endif

    area = &zone->free_area[current_order];
    if (unlikely(list_empty(&area->free_list[migratetype])))
      continue;

    page = list_entry(area->free_list[migratetype].next, struct page, list);
#ifdef CONFIG_MM_PMM_SPECULATIVE_PREFETCH
    __builtin_prefetch(page, 1, 3);
#endif
    list_del(&page->list);

    ClearPageBuddy(page);
    area->nr_free--;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
    clear_free_area_bit(zone, current_order, migratetype);
#endif
    zone->nr_free_pages -= (1UL << current_order);

    if (unlikely(current_order == zone->max_free_order && area->nr_free == 0)) {
      int o = current_order;
      while (o > 0 && zone->free_area[o].nr_free == 0)
        o--;
      zone->max_free_order = o;
    }

    expand(zone, page, order, current_order, area, migratetype);
    page->migratetype = migratetype;
    return page;
  }

  return __rmqueue_fallback(zone, order, migratetype);
}

static void flush_deferred_pages(struct zone *zone);
static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order,
                            int migratetype);

int rmqueue_bulk(struct zone *zone, unsigned int order, unsigned int count,
                 struct list_head *list, int migratetype) {
  int i;

  if (!zone || !zone->present_pages || count == 0)
    return 0;

  spinlock_lock(&zone->lock);

#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
  if (zone->deferred_count > 0) {
    flush_deferred_pages(zone);
  }
#endif

  for (i = 0; i < (int)count; ++i) {
    struct page *page = __rmqueue(zone, order, migratetype);
    if (unlikely(page == nullptr))
      break;

    list_add_tail(&page->list, list);
  }

  spinlock_unlock(&zone->lock);
  return i;
}

/**
 * drain_zone_pages - Return a batch of pages from a PCP list to the buddy
 * system. This is the core of the "Batched PCP" optimization.
 */
static void drain_zone_pages(struct zone *zone, struct list_head *list,
                             int count, int order) {
  unsigned long flags;
  struct page *page;

  flags = spinlock_lock_irqsave(&zone->lock);

  while (count-- > 0 && !list_empty(list)) {
    page = list_first_entry(list, struct page, list);
    list_del(&page->list);
    __free_one_page(page, (unsigned long)(page - mem_map), zone, order,
                    page->migratetype);
  }

  spinlock_unlock_irqrestore(&zone->lock, flags);
}

void free_pcp_pages(struct zone *zone, int count, struct list_head *list,
                    int order) {
  drain_zone_pages(zone, list, count, order);
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order,
                            int migratetype) {
#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
  if (order == 0 && zone->deferred_count < DEFERRED_BATCH_SIZE) {
    page->order = order;
    page->migratetype = migratetype;
    list_add(&page->list, &zone->deferred_list);
    zone->deferred_count++;
    return;
  }

  if (zone->deferred_count >= DEFERRED_BATCH_SIZE) {
    flush_deferred_pages(zone);
  }
#endif

  unsigned long buddy_pfn;
  struct page *buddy;
  unsigned long combined_pfn;

  if (unlikely(PageBuddy(page))) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free detected: pfn %lu", pfn);
    panic(buf);
  }

  while (order < MAX_ORDER - 1) {
    buddy_pfn = __find_buddy_pfn(pfn, order);
    buddy = page + (long)(buddy_pfn - pfn);

#ifdef CONFIG_MM_PMM_SPECULATIVE_PREFETCH
    __builtin_prefetch(buddy, 0, 3);
#endif

    if (!page_is_buddy(page, buddy, order))
      break;

    list_del(&buddy->list);
    zone->free_area[order].nr_free--;
    zone->nr_free_pages -= (1UL << order);
    ClearPageBuddy(buddy);
    buddy->order = 0;

    combined_pfn = buddy_pfn & pfn;
    page = page + (long)(combined_pfn - pfn);
    pfn = combined_pfn;
    order++;
  }

  SetPageBuddy(page);
  page->order = order;
  page->migratetype = migratetype;
  list_add(&page->list, &zone->free_area[order].free_list[migratetype]);
  zone->free_area[order].nr_free++;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
  set_free_area_bit(zone, order, migratetype);
#endif
  zone->nr_free_pages += (1UL << order);
  
  if ((int)order > (int)zone->max_free_order)
    zone->max_free_order = order;
}

#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
static void flush_deferred_pages(struct zone *zone) {
  struct page *page, *tmp;

  list_for_each_entry_safe(page, tmp, &zone->deferred_list, list) {
    list_del(&page->list);
    unsigned long pfn = (unsigned long)(page - mem_map);
    unsigned long buddy_pfn;
    struct page *buddy;
    unsigned long combined_pfn;
    unsigned int order = page->order;
    int migratetype = page->migratetype;

    while (order < MAX_ORDER - 1) {
      buddy_pfn = __find_buddy_pfn(pfn, order);
      buddy = page + (long)(buddy_pfn - pfn);
      if (!page_is_buddy(page, buddy, order))
        break;
      list_del(&buddy->list);
      zone->free_area[order].nr_free--;
      zone->nr_free_pages -= (1UL << order);
      ClearPageBuddy(buddy);
      buddy->order = 0;
      combined_pfn = buddy_pfn & pfn;
      page = page + (long)(combined_pfn - pfn);
      pfn = combined_pfn;
      order++;
    }

    SetPageBuddy(page);
    page->order = order;
    page->migratetype = migratetype;
    list_add(&page->list, &zone->free_area[order].free_list[migratetype]);
    zone->free_area[order].nr_free++;
#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
    set_free_area_bit(zone, order, migratetype);
#endif
    zone->nr_free_pages += (1UL << order);
    if (order > zone->max_free_order)
      zone->max_free_order = order;
  }
  zone->deferred_count = 0;
}
#endif

extern size_t shrink_inactive_list(size_t nr_to_scan);

extern int numa_distance_get(int from, int to);

static int sorted_nodes[MAX_NUMNODES][MAX_NUMNODES];
static int active_nodes[MAX_NUMNODES];
static int nr_active_nodes = 0;

static void build_zonelists_node(struct pglist_data *pgdat,
                                 struct zonelist *zonelist, int nr_zones) {
  int zone_idx = 0;
  int nid = pgdat->node_id;

  for (int z = nr_zones; z >= 0; z--) {
    struct zone *zone = &pgdat->node_zones[z];
    if (zone->present_pages) {
      zonelist->_zones[zone_idx++] = zone;
    }
  }

  for (int i = 0; i < nr_active_nodes; i++) {
    int node = sorted_nodes[nid][i];
    if (node == nid)
      continue;

    struct pglist_data *remote_pgdat = node_data[node];
    for (int z = nr_zones; z >= 0; z--) {
      struct zone *zone = &remote_pgdat->node_zones[z];
      if (zone->present_pages) {
        zonelist->_zones[zone_idx++] = zone;
      }
    }
  }

  zonelist->_zones[zone_idx] = nullptr;
}

void build_all_zonelists(void) {
  nr_active_nodes = 0;
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (node_data[i])
      active_nodes[nr_active_nodes++] = i;
  }

  for (int from = 0; from < nr_active_nodes; from++) {
    int from_nid = active_nodes[from];

    for (int i = 0; i < nr_active_nodes; i++) {
      int to_nid = active_nodes[i];
      int dist = (from_nid == to_nid) ? 0 : numa_distance_get(from_nid, to_nid);

      int j = i;
      while (j > 0) {
        int prev_nid = sorted_nodes[from_nid][j - 1];
        int prev_dist =
            (from_nid == prev_nid) ? 0 : numa_distance_get(from_nid, prev_nid);
        if (prev_dist <= dist)
          break;
        sorted_nodes[from_nid][j] = sorted_nodes[from_nid][j - 1];
        j--;
      }
      sorted_nodes[from_nid][j] = to_nid;
    }
  }

  for (int n = 0; n < nr_active_nodes; n++) {
    int nid = active_nodes[n];
    for (int z = 0; z < MAX_NR_ZONES; z++) {
      build_zonelists_node(node_data[nid], &node_data[nid]->node_zonelists[z],
                           z);
    }
  }

  printk(KERN_INFO PMM_CLASS "Built zonelists for all nodes.\n");
}

#ifdef CONFIG_MM_PMM_WATERMARK_BOOST_DECAY
static void decay_watermark_boost(struct zone *z) {
  if (z->watermark_boost == 0)
    return;

  uint64_t now = get_time_ns();
  uint64_t elapsed = now - z->last_boost_decay_time;

  if (elapsed > 1000) {
    z->watermark_boost = (z->watermark_boost * z->watermark_boost_factor) / 100;
    if (z->watermark_boost < (z->present_pages / 1000))
      z->watermark_boost = 0;

    z->watermark_boost_factor =
        z->watermark_boost_factor > 10 ? z->watermark_boost_factor - 5 : 5;
    z->last_boost_decay_time = now;
  }
}
#endif

#ifdef CONFIG_MM_PMM_FRAGMENTATION_INDEX
static void calculate_fragmentation_index(struct zone *z) {
  uint64_t now = get_time_ns();
  if (now - z->last_frag_calc_time < 5000)
    return;

  unsigned long free_pages = z->nr_free_pages;
  if (free_pages == 0) {
    z->fragmentation_index = 1000;
    z->last_frag_calc_time = now;
    return;
  }

  unsigned long usable = 0;
  for (unsigned int order = 0; order < MAX_ORDER; order++) {
    usable += z->free_area[order].nr_free * (1UL << order);
  }

  z->fragmentation_index =
      (unsigned int)((1000 * (free_pages - usable)) / free_pages);
  z->last_frag_calc_time = now;
}
#endif

/*
 * Watermark check helper with dirty page awareness
 */
static bool zone_watermark_ok(struct zone *z, unsigned int order,
                              unsigned long mark, int classzone_idx,
                              unsigned int alloc_flags) {
  long free_pages = (long)__atomic_load_n(&z->nr_free_pages, __ATOMIC_ACQUIRE);
  long min = (long)mark;

#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
  min += z->watermark_boost;
#endif

#ifdef CONFIG_MM_PMM_DIRTY_TRACKING
  long dirty = atomic_long_read(&z->nr_dirty);
  long dirty_limit = z->present_pages / 10;
  if (dirty > dirty_limit) {
    min += (dirty - dirty_limit) / 2;
    atomic_long_inc(&z->dirty_exceeded_count);
  }
#endif

#ifdef CONFIG_MM_PMM_HIGHATOMIC
  if (alloc_flags & ALLOC_HIGHATOMIC)
    min -= z->nr_reserved_highatomic / 2;
#endif

  if (free_pages <= min + (1UL << order))
    return false;

  for (unsigned int o = order; o < MAX_ORDER; o++) {
    if (z->free_area[o].nr_free > 0)
      return true;
  }

  return false;
}

/*
 * Core Allocator
 */
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  struct page *page = nullptr;
  struct pglist_data *pgdat = nullptr;
  struct zone *z;
  unsigned long flags;
  bool can_reclaim = gfpflags_allow_blocking(gfp_mask);
  int reclaim_retries = 3;
  int migratetype = gfp_to_migratetype(gfp_mask);
  struct resdomain *rd = nullptr;

  /*
   * Resource Domain Charge
   * Attempt to charge the domain before we do any heavy lifting.
   */
  if (current && !(gfp_mask & ___GFP_NO_CHARGE)) {
    rd = current->rd;
    if (rd) {
      if (resdomain_charge_mem(rd, (1UL << order) * PAGE_SIZE, false) < 0) {
        return nullptr;
      }
    }
  }

#ifdef CONFIG_MM_PMM_FAIR_ALLOC
  static atomic_t zone_rotator = ATOMIC_INIT(0);
  int rotation = atomic_inc_return(&zone_rotator);
#endif

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
  if (gfp_mask & ___GFP_DMA)
    start_zone_idx = ZONE_DMA;
  else if (gfp_mask & ___GFP_DMA32)
    start_zone_idx = ZONE_DMA32;

  pgdat = node_data[nid];

  /*
   * PCP Fastpath (Orders 0-3, Local Node)
   */
  if (order < PCP_ORDERS && percpu_ready()) {
    for (int i = start_zone_idx; i >= 0; i--) {
      z = &pgdat->node_zones[i];
      if (!z->present_pages)
        continue;

      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int)smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

#ifdef CONFIG_MM_PMM_PCP_DYNAMIC
      if (pcp->count > pcp->high * 2) {
        pcp->batch = min(pcp->batch_max, pcp->batch * 2);
        pcp->high = min(pcp->high_max, pcp->high + pcp->batch);
      } else if (pcp->count < pcp->high / 4 && pcp->batch > pcp->batch_min) {
        pcp->batch = max(pcp->batch_min, pcp->batch / 2);
        pcp->high = max(pcp->high_min, pcp->high - pcp->batch);
      }
#endif

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
      int list_idx = PCP_LIST_HOT;
      if (list_empty(&pcp->lists[order][list_idx]))
        list_idx = PCP_LIST_COLD;

      if (list_empty(&pcp->lists[order][list_idx]))
#else
      if (list_empty(&pcp->lists[order][0]))
#endif
      {
        if (z->nr_free_pages >= z->watermark[WMARK_LOW]) {
#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
          int count =
              rmqueue_bulk(z, order, pcp->batch,
                           &pcp->lists[order][PCP_LIST_COLD], migratetype);
#else
          int count = rmqueue_bulk(z, order, pcp->batch, &pcp->lists[order][0],
                                   migratetype);
#endif
          pcp->count += count;
#ifdef CONFIG_MM_PMM_STATS
          atomic_long_inc(&pcp->refill_count);
#endif
        }
      }

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
      if (!list_empty(&pcp->lists[order][list_idx])) {
        page =
            list_first_entry(&pcp->lists[order][list_idx], struct page, list);
#else
      if (!list_empty(&pcp->lists[order][0])) {
        page = list_first_entry(&pcp->lists[order][0], struct page, list);
#endif
        list_del(&page->list);
        pcp->count--;

        check_page_poison(page, 1 << order);
        if (gfp_mask & __GFP_ZERO) {
          memset(page_address(page), 0, (size_t)(1UL << order) << PAGE_SHIFT);
        } else {
          kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);
        }

        struct folio *folio = (struct folio *)page;
        folio->order = (uint16_t)order;
        folio->node = page->node;
        folio->zone = page->zone;
        SetPageHead(&folio->page);
        atomic_set(&folio->_refcount, 1);
        page->rd = rd;

#ifdef CONFIG_MM_PMM_STATS
        atomic_long_inc(&z->alloc_success);
        atomic_long_inc(&pcp->alloc_count);
#endif
        restore_irq_flags(irq_flags);
        return folio;
      }
      restore_irq_flags(irq_flags);
    }
  }

  /*
   * Zonelist Traversal
   */
  struct zonelist *zonelist = &pgdat->node_zonelists[start_zone_idx];
  struct zone **z_ptr = zonelist->_zones;
  int z_count = 0;
  while (zonelist->_zones[z_count])
    z_count++;

#ifdef CONFIG_MM_PMM_FAIR_ALLOC
  /* Simple fair rotation: start from a different zone in the list */
  if (z_count > 1) {
    z_ptr = &zonelist->_zones[rotation % z_count];
  }
#endif

  int zones_tried = 0;
  while (zones_tried < z_count) {
    z = *z_ptr++;
    zones_tried++;

    if (!z) {
      /* Wrap around to the beginning of the zonelist */
      z_ptr = zonelist->_zones;
      z = *z_ptr++;
    }

    if (!z || !z->present_pages || order > z->max_free_order)
      continue;

    /* Check watermarks */
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST_DECAY
    decay_watermark_boost(z);
#endif
#ifdef CONFIG_MM_PMM_FRAGMENTATION_INDEX
    calculate_fragmentation_index(z);
#endif

    if (!zone_watermark_ok(z, order, z->watermark[WMARK_LOW], start_zone_idx,
                           0)) {
      wakeup_kswapd(z);
      if (!can_reclaim && !zone_watermark_ok(z, order, z->watermark[WMARK_MIN],
                                             start_zone_idx, 0))
        continue;
    }

    flags = spinlock_lock_irqsave(&z->lock);
#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
    if (z->deferred_count > 0) {
      flush_deferred_pages(z);
    }
#endif
    page = __rmqueue(z, order, migratetype);
    spinlock_unlock_irqrestore(&z->lock, flags);

    if (page) {
#ifdef CONFIG_MM_PMM_STATS
      atomic_long_inc(&z->alloc_success);
#endif
      goto found;
    } else if (order > 0) {
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
      z->watermark_boost += (1UL << order);
      if (z->watermark_boost > z->present_pages / 4)
        z->watermark_boost = z->present_pages / 4;
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST_DECAY
      z->watermark_boost_factor = 100;
      z->last_boost_decay_time = get_time_ns();
#endif
#endif
#ifdef CONFIG_MM_PMM_STATS
      atomic_long_inc(&z->fallback_count);
#endif
    }
  }

  /*
   * Direct Reclaim
   */
  if (can_reclaim && reclaim_retries > 0) {
    size_t reclaimed = try_to_free_pages(pgdat, 32, gfp_mask);
    if (reclaimed > 0) {
#ifdef CONFIG_MM_PMM_STATS
      // We don't have easy access to the specific zone here, but could track
      // per-node
#endif
      reclaim_retries--;
      goto retry;
    }
  }

#ifdef CONFIG_MM_PMM_STATS
  // Track failure for the primary zone
  atomic_long_inc(&pgdat->node_zones[start_zone_idx].alloc_fail);
#endif

  printk(KERN_ERR PMM_CLASS
         "failed to allocate order %u from any node (gfp: %x)\n",
         order, gfp_mask);
  
  if (rd) resdomain_uncharge_mem(rd, (1UL << order) * PAGE_SIZE);
  return nullptr;

found:
  check_page_sanity(page, order);

  /* Verify the page wasn't corrupted while on the free list */
  check_page_poison(page, 1 << order);
  
  if (gfp_mask & __GFP_ZERO) {
    memset(page_address(page), 0, (size_t)(1UL << order) << PAGE_SHIFT);
  } else {
    /* Poison as allocated */
    kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);
  }

  struct folio *folio = (struct folio *)page;

  folio->order = (uint16_t)order;
  folio->node = page->node; /* Use actual page node */
  folio->zone = page->zone;
  SetPageHead(&folio->page);
  page->rd = rd;

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

unsigned long alloc_pages_bulk_array(int nid, gfp_t gfp_mask,
                                     unsigned int order, unsigned long nr_pages,
                                     struct page **pages_array) {
  if (!nr_pages)
    return 0;
  if (nid < 0)
    nid = 0; // fallback default
  if (!node_data[nid]) {
    /* find valid node */
    for (int i = 0; i < MAX_NUMNODES; i++)
      if (node_data[i]) {
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
    if (gfp_mask & ___GFP_DMA)
      zone_idx = ZONE_DMA;

    z = &pgdat->node_zones[zone_idx];
    if (z->present_pages) {
      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int)smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

      while (allocated < nr_pages) {
#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
        int list_idx = PCP_LIST_HOT;
        if (list_empty(&pcp->lists[order][list_idx]))
          list_idx = PCP_LIST_COLD;

        if (list_empty(&pcp->lists[order][list_idx]))
#else
        if (list_empty(&pcp->lists[order][0]))
#endif
        {
          int batch = max((int)pcp->batch, (int)(nr_pages - allocated));
          if (batch > pcp->high)
            batch = pcp->high;

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
          int count = rmqueue_bulk(
              z, order, batch, &pcp->lists[order][PCP_LIST_COLD], migratetype);
#else
          int count =
              rmqueue_bulk(z, order, batch, &pcp->lists[order][0], migratetype);
#endif
          pcp->count += count;
#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
          if (list_empty(&pcp->lists[order][list_idx]))
            break;
#else
          if (list_empty(&pcp->lists[order][0]))
            break;
#endif
        }

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
        struct page *page =
            list_first_entry(&pcp->lists[order][list_idx], struct page, list);
#else
        struct page *page =
            list_first_entry(&pcp->lists[order][0], struct page, list);
#endif
        list_del(&page->list);
        pcp->count--;

        check_page_poison(page, 1 << order);
        if (gfp_mask & __GFP_ZERO) {
          memset(page_address(page), 0, (size_t)(1UL << order) << PAGE_SHIFT);
        } else {
          kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);
        }

        struct folio *folio = (struct folio *)page;
        folio->order = (uint16_t)order;
        folio->node = page->node;
        folio->zone = page->zone;
        SetPageHead(&folio->page);
        atomic_set(&folio->_refcount, 1);

        pages_array[allocated++] = page;
      }
      restore_irq_flags(irq_flags);
    }
  }

  if (allocated == nr_pages)
    return allocated;

  /* Slow Path: Zone Lock */
  // We just pick the Normal zone for now.
  z = &pgdat->node_zones[ZONE_NORMAL];
  if (!z->present_pages)
    z = &pgdat->node_zones[ZONE_DMA32];

  if (z->present_pages) {
    flags = spinlock_lock_irqsave(&z->lock);
    while (allocated < nr_pages) {
      struct page *page = __rmqueue(z, order, migratetype);
      if (!page)
        break;

      check_page_sanity(page, order);

      check_page_poison(page, 1 << order);
      if (gfp_mask & __GFP_ZERO) {
        memset(page_address(page), 0, (size_t)(1UL << order) << PAGE_SHIFT);
      } else {
        kernel_poison_pages(page, 1 << order, PAGE_POISON_ALLOC);
      }

      struct folio *folio = (struct folio *)page;
      folio->order = (uint16_t)order;
      folio->node = page->node;
      folio->zone = page->zone;
      SetPageHead(&folio->page);

      /* Handle higher orders tail pages if needed, omitted for bulk 0-order opt
       */
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
  if (!nr_pages)
    return;

  for (unsigned long i = 0; i < nr_pages; i++) {
    struct page *page = pages[i];
    if (!page)
      continue;

    int order = page->order;
    struct zone *z = &managed_zones[page->zone];
    if (node_data[page->node]) {
      z = &node_data[page->node]->node_zones[page->zone];
    }

    /* Try PCP first */
    if (order < PCP_ORDERS && percpu_ready()) {
      irq_flags_t irq_flags = save_irq_flags();
      int cpu = (int)smp_get_id();
      struct per_cpu_pages *pcp = &z->pageset[cpu];

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
      list_add(&page->list, &pcp->lists[order][PCP_LIST_HOT]);
#else
      list_add(&page->list, &pcp->lists[order][0]);
#endif
      pcp->count++;

      if (pcp->count >= pcp->high) {
#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
        drain_zone_pages(z, &pcp->lists[order][PCP_LIST_COLD], pcp->batch,
                         order);
#else
        drain_zone_pages(z, &pcp->lists[order][0], pcp->batch, order);
#endif
        pcp->count -= pcp->batch;
      }
      restore_irq_flags(irq_flags);
      continue;
    }

    /* Slow path */
    irq_flags_t flags = spinlock_lock_irqsave(&z->lock);
    __free_one_page(page, (unsigned long)(page - mem_map), z, order,
                    page->migratetype);
    spinlock_unlock_irqrestore(&z->lock, flags);
  }
}

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order) {
  int nid = 0;
  if (current)
    nid = current->node_id;

  return alloc_pages_node(nid, gfp_mask, order);
}
EXPORT_SYMBOL(alloc_pages);

void put_page(struct page *page) {
  if (!page || PageReserved(page))
    return;

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
      __free_pages(&folio->page, order);
    } else {
      __free_pages(&folio->page, 0);
    }
  }
}
EXPORT_SYMBOL(put_page);

/**
 * __free_pages_boot_core - Boot-time page freeing, no locking or poisoning.
 * @page:  Head page of the block.
 * @order: Buddy order.
 *
 * This is used exclusively during pmm_init() to populate the buddy free
 * lists from bootloader-provided usable memory.  It deliberately skips:
 *
 *   - Page poisoning:      Pages haven't been used; nothing to poison.
 *   - PCP / deferred:      Per-CPU structures are not ready at boot.
 *   - Locking:             BSP is single-threaded during pmm_init().
 *   - IRQ save/restore:    Interrupts are disabled at boot.
 *   - Double-free check:   Only called once per page from pmm_init().
 *
 * Context: Boot only.  Must NOT be called after pmm_initialized is set.
 * Locking: None.  Single-threaded boot path only.
 */
void __free_pages_boot_core(struct page *page, unsigned int order) {
  unsigned long pfn = (unsigned long)(page - mem_map);
  struct pglist_data *pgdat = node_data[page->node];
  if (!pgdat)
    pgdat = node_data[0];
  struct zone *zone = &pgdat->node_zones[page->zone];

  __free_one_page(page, pfn, zone, order, page->migratetype);
}

void __free_pages(struct page *page, unsigned int order) {
  if (!page)
    return;

  if (unlikely(PageBuddy(page))) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free of page %p", page);
    panic(buf);
  }

  /* Resource Domain Uncharge */
  if (page->rd) {
      resdomain_uncharge_mem(page->rd, (1UL << order) * PAGE_SIZE);
      page->rd = nullptr;
  }

  /* Poison the page being freed */
  kernel_poison_pages(page, 1 << order, PAGE_POISON_FREE);

  /*
   * PCP optimization for small orders (0-3).
   * This is the hot path for single page frees.
   */
  if (order < PCP_ORDERS && percpu_ready()) {
    irq_flags_t flags = save_irq_flags();
    int cpu = (int)smp_get_id();

    struct pglist_data *pgdat = node_data[page->node];
    struct zone *zone = &pgdat->node_zones[page->zone];
    struct per_cpu_pages *pcp = &zone->pageset[cpu];

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
    list_add(&page->list, &pcp->lists[order][PCP_LIST_HOT]);
#else
    list_add(&page->list, &pcp->lists[order][0]);
#endif
    pcp->count++;

#ifdef CONFIG_MM_PMM_STATS
    atomic_long_inc(&pcp->free_count);
#endif

    if (pcp->count >= pcp->high) {
      int to_drain = pcp->batch;
      if (to_drain > pcp->count)
        to_drain = pcp->count;

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
      drain_zone_pages(zone, &pcp->lists[order][PCP_LIST_COLD], to_drain,
                       order);
#else
      drain_zone_pages(zone, &pcp->lists[order][0], to_drain, order);
#endif
      pcp->count -= to_drain;
#ifdef CONFIG_MM_PMM_STATS
      atomic_long_inc(&pcp->drain_count);
#endif
    }

    restore_irq_flags(flags);
    return;
  }

  struct pglist_data *pgdat = node_data[page->node];
  if (!pgdat)
    pgdat = node_data[0];
  struct zone *zone = &pgdat->node_zones[page->zone];

  unsigned long flags;
  unsigned long pfn = (unsigned long)(page - mem_map);
  flags = spinlock_lock_irqsave(&zone->lock);
  __free_one_page(page, pfn, zone, order, page->migratetype);
  spinlock_unlock_irqrestore(&zone->lock, flags);
}
EXPORT_SYMBOL(__free_pages);

void pmm_verify(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;
    struct pglist_data *pgdat = node_data[n];

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      if (!z->present_pages)
        continue;

      irq_flags_t flags = spinlock_lock_irqsave(&z->lock);
      unsigned long found_free = 0;

      for (int order = 0; order < MAX_ORDER; order++) {
        for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
          struct page *page;
          list_for_each_entry(page, &z->free_area[order].free_list[mt], list) {
            if (unlikely(!PageBuddy(page))) {
              panic("PMM: Page in free list without PageBuddy set! (pfn %lu)\n",
                    (unsigned long)(page - mem_map));
            }
            if (unlikely(page->order != order)) {
              panic("PMM: Page in free list with wrong order! (expected %d, "
                    "got %d)\n",
                    order, page->order);
            }
            found_free += (1UL << order);
          }
        }
      }

      if (unlikely(found_free != z->nr_free_pages)) {
        panic("PMM: Free page count mismatch in zone %s! (found %lu, expected "
              "%lu)\n",
              z->name, found_free, z->nr_free_pages);
      }
      spinlock_unlock_irqrestore(&z->lock, flags);
    }
  }
}

void free_area_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;

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

#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
      for (int order = 0; order < MAX_ORDER; order++) {
        z->free_area_bitmap[order] = 0;
      }
#endif

#ifdef CONFIG_MM_PMM_PAGEBLOCK_METADATA
      unsigned long nr_pageblocks =
          (z->spanned_pages + PAGEBLOCK_NR_PAGES - 1) / PAGEBLOCK_NR_PAGES;
      unsigned long bitmap_size = (nr_pageblocks * PAGEBLOCK_BITS + 63) / 64;
      z->pageblock_flags = nullptr;
#endif

#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
      INIT_LIST_HEAD(&z->deferred_list);
      z->deferred_count = 0;
#endif

#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
      z->watermark_boost = 0;
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST_DECAY
      z->watermark_boost_factor = 100;
      z->last_boost_decay_time = 0;
#endif
#endif

#ifdef CONFIG_MM_PMM_DIRTY_TRACKING
      atomic_long_set(&z->nr_dirty, 0);
      atomic_long_set(&z->dirty_exceeded_count, 0);
#endif

#ifdef CONFIG_MM_PMM_FRAGMENTATION_INDEX
      z->fragmentation_index = 0;
      z->last_frag_calc_time = 0;
#endif

#ifdef CONFIG_MM_PMM_HIGHATOMIC
      z->nr_reserved_highatomic = 0;
#endif

      for (int order = 0; order < MAX_ORDER; order++) {
        for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
          INIT_LIST_HEAD(&z->free_area[order].free_list[mt]);
        }
        z->free_area[order].nr_free = 0;
      }
    }
  }
}

unsigned long nr_free_pages(void) {
    unsigned long total = 0;
    for (int n = 0; n < MAX_NUMNODES; n++) {
        if (!node_data[n]) continue;
        for (int i = 0; i < MAX_NR_ZONES; i++) {
            total += node_data[n]->node_zones[i].nr_free_pages;
        }
    }
    return total;
}
