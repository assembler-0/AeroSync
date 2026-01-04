///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/page_alloc.c
 * @brief Zone allocator
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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
#include <kernel/spinlock.h>
#include <kernel/panic.h>
#include <arch/x86_64/cpu.h>
#include <kernel/classes.h>
#include <linux/list.h>
#include <linux/container_of.h>
#include <mm/gfp.h>
#include <kernel/sched/sched.h>

/* Global zones */
struct zone managed_zones[MAX_NR_ZONES];

DECLARE_PER_CPU(struct per_cpu_pages, pcp_pages);

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
  return page_pfn ^ (1 << order);
}

static inline bool page_is_buddy(struct page *page, struct page *buddy, unsigned int order) {
  if (!PageBuddy(buddy))
    return false;
  if (buddy->order != order)
    return false;
  return true;
}

static inline void expand(struct zone *zone, struct page *page,
                          int low, int high, struct free_area *area) {
  unsigned long size = 1 << high;

  while (high > low) {
    area--;
    high--;
    size >>= 1;

    struct page *buddy = page + size;
    INIT_LIST_HEAD(&buddy->list);

    SetPageBuddy(buddy);
    buddy->order = high;

    list_add(&buddy->list, &area->free_list[0]);
    area->nr_free++;
    zone->nr_free_pages += size;
    if (high > (int) zone->max_free_order) zone->max_free_order = high;
  }
}

static struct page *__rmqueue(struct zone *zone, unsigned int order) {
  unsigned int current_order;
  struct free_area *area;
  struct page *page;

  for (current_order = order; current_order < MAX_ORDER; ++current_order) {
    area = &zone->free_area[current_order];
    if (list_empty(&area->free_list[0]))
      continue;

    page = list_entry(area->free_list[0].next, struct page, list);
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

    expand(zone, page, order, current_order, area);
    return page;
  }

  return NULL;
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order);

int rmqueue_bulk(struct zone *zone, unsigned int order, unsigned int count,
                 struct list_head *list) {
  int i;
  
  /* Validate zone boundaries before bulk allocation */
  if (!zone || !zone->present_pages || count == 0) return 0;
  
  spinlock_lock(&zone->lock);
  
  for (i = 0; i < (int) count; ++i) {
    struct page *page = __rmqueue(zone, order);
    if (unlikely(page == NULL))
      break;
    
    list_add_tail(&page->list, list);
  }
  
  spinlock_unlock(&zone->lock);
  return i;
}

void free_pcp_pages(struct zone *zone, int count, struct list_head *list) {
  spinlock_lock(&zone->lock);
  while (count--) {
    struct page *page = list_first_entry(list, struct page, list);
    list_del(&page->list);
    __free_one_page(page, page_to_pfn(page), zone, 0);
  }
  spinlock_unlock(&zone->lock);
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order) {
  unsigned long buddy_pfn;
  struct page *buddy;

  // Validate page before starting merge process
  if (PageBuddy(page)) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free detected: pfn %lu", pfn);
    panic(buf);
  }

  while (order < MAX_ORDER - 1) {
    buddy_pfn = __find_buddy_pfn(pfn, order);

    if (buddy_pfn >= zone->zone_start_pfn + zone->spanned_pages)
      break;

    // Check if buddy is within max pages (global check)
    if (buddy_pfn >= pmm_max_pages)
      break;

    buddy = &mem_map[buddy_pfn];

    // Comprehensive buddy validation
    if (!page_is_buddy(page, buddy, order))
      break;
      
    // Additional safety: verify buddy is in same zone
    if (buddy->zone != page->zone)
      break;
      
    // Verify buddy is not corrupted
    if (buddy->order != order) {
      printk(KERN_ERR PMM_CLASS "Buddy order mismatch: expected %u, got %u\n", 
             order, buddy->order);
      break;
    }

    /* Our buddy is free, merge with it */
    list_del(&buddy->list);
    zone->free_area[order].nr_free--;
    zone->nr_free_pages -= (1UL << order);
    ClearPageBuddy(buddy);
    buddy->order = 0; // Clear buddy order

    pfn &= ~(1UL << order);
    page = &mem_map[pfn];
    order++;
  }

  SetPageBuddy(page);
  page->order = order;
  list_add(&page->list, &zone->free_area[order].free_list[0]);
  zone->free_area[order].nr_free++;
  zone->nr_free_pages += (1UL << order);
  if (order > zone->max_free_order) zone->max_free_order = order;
}

extern size_t shrink_inactive_list(size_t nr_to_scan);

/*
 * Core Allocator
 */
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  struct page *page = NULL;
  struct pglist_data *pgdat = NULL;
  struct zone *z;
  int z_idx;
  unsigned long flags;
  bool can_reclaim = !(gfp_mask & ___GFP_ATOMIC);
  int reclaim_retries = 3;

retry:
  // Validate and fallback NUMA node (moved outside retry loop)
  if (nid < 0 || nid >= MAX_NUMNODES || !node_data[nid]) {
    // Find first valid node instead of assuming node 0
    nid = -1;
    for (int i = 0; i < MAX_NUMNODES; i++) {
      if (node_data[i]) {
        nid = i;
        break;
      }
    }
    if (nid == -1) {
      printk(KERN_ERR PMM_CLASS "No valid NUMA nodes available\n");
      return NULL;
    }
  }

  int start_zone = ZONE_NORMAL;
  if (gfp_mask & ___GFP_DMA) start_zone = ZONE_DMA;
  else if (gfp_mask & ___GFP_DMA32) start_zone = ZONE_DMA32;

  /*
   * PCP Fastpath (Order 0, Normal/HighMem, Local Node)
   */
  if (order == 0 && percpu_ready() && (start_zone == ZONE_NORMAL) && (nid == this_node())) {
      irq_flags_t irq_flags = save_irq_flags();
      struct per_cpu_pages *pcp = this_cpu_ptr(pcp_pages);
      
      /* Ensure pgdat is set for 'found' label */
      pgdat = node_data[nid];
      if (unlikely(!pgdat)) {
          restore_irq_flags(irq_flags);
          return NULL;
      }

      if (list_empty(&pcp->list)) {
          /* Refill from Normal Zone */
          struct zone *refill_zone = &pgdat->node_zones[ZONE_NORMAL];
          
          if (refill_zone->nr_free_pages >= refill_zone->watermark[WMARK_LOW]) {
             int count = rmqueue_bulk(refill_zone, 0, pcp->batch, &pcp->list);
             pcp->count += count;
          }
      }

      if (!list_empty(&pcp->list)) {
          page = list_first_entry(&pcp->list, struct page, list);
          list_del(&page->list);
          pcp->count--;
          restore_irq_flags(irq_flags);
          goto found;
      }
      restore_irq_flags(irq_flags);
  }

  /*
   * Try preferred node first
   */
  pgdat = node_data[nid];
  for (z_idx = start_zone; z_idx >= 0; z_idx--) {
    z = &pgdat->node_zones[z_idx];

    if (!z->present_pages || order > z->max_free_order) continue;

  /* Check watermarks with atomic operations */
  if (__atomic_load_n(&z->nr_free_pages, __ATOMIC_ACQUIRE) < z->watermark[WMARK_LOW]) {
    wakeup_kswapd(z);
  }

  /* If we are under the MIN watermark and can't reclaim, we might fail unless HIGH priority */
  if (__atomic_load_n(&z->nr_free_pages, __ATOMIC_ACQUIRE) < z->watermark[WMARK_MIN] && 
      !can_reclaim && !(gfp_mask & ___GFP_HIGH)) {
      continue;
  }

    flags = spinlock_lock_irqsave(&z->lock);
    page = __rmqueue(z, order);
    spinlock_unlock_irqrestore(&z->lock, flags);

    if (page) {
      goto found;
    }
  }

  /*
   * Fallback to other nodes
   */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (i == nid || !node_data[i]) continue;
    pgdat = node_data[i];
    for (z_idx = start_zone; z_idx >= 0; z_idx--) {
      z = &pgdat->node_zones[z_idx];
      if (!z->present_pages || order > z->max_free_order) continue;
      
      flags = spinlock_lock_irqsave(&z->lock);
      page = __rmqueue(z, order);
      spinlock_unlock_irqrestore(&z->lock, flags);
      if (page) goto found;
    }
  }

  /*
   * Direct Reclaim / Demand Allocation
   * If we are allowed to sleep/reclaim, try to free some pages and retry.
   */
  if (can_reclaim && reclaim_retries > 0) {
      // Try to free 32 pages (SWAP_CLUSTER_MAX equivalent)
      size_t reclaimed = shrink_inactive_list(32);
      
      if (reclaimed > 0) {
          reclaim_retries--;
          goto retry;
      }
      
      // If we couldn't reclaim anything, maybe OOM or just everything active.
      // We could try compacting here if implemented.
  }

  printk(KERN_ERR PMM_CLASS "failed to allocate order %u from any node (gfp: %x)\n", order, gfp_mask);
  return NULL;

found:
  check_page_sanity(page, order);
  page->order = order;
  page->node = pgdat->node_id;
  SetPageHead(page);

  /* Initialize tail pages if order > 0 */
  if (order > 0) {
    size_t nr = 1UL << order;
    for (size_t i = 1; i < nr; i++) {
      struct page *tail = page + i;
      tail->flags = 0;
      SetPageTail(tail);
      tail->head = page;
      tail->node = pgdat->node_id;
    }
  }

  atomic_set(&page->_refcount, 1);
  return (struct folio *) page;
}

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order) {
  int nid = 0;
  if (current) nid = current->node_id;

  return alloc_pages_node(nid, gfp_mask, order);
}

void put_page(struct page *page) {
  if (!page || PageReserved(page)) return;

  struct folio *folio = page_folio(page);
  page = &folio->page;

  if (atomic_dec_and_test(&page->_refcount)) {
    unsigned int order = 0;
    if (PageHead(page)) {
      order = page->order;
      ClearPageHead(page);

      /* Cleanup tail pages */
      if (order > 0) {
        size_t nr = 1UL << order;
        for (size_t i = 1; i < nr; i++) {
          struct page *tail = page + i;
          ClearPageTail(tail);
          tail->head = NULL;
        }
      }
    }
    __free_pages(page, order);
  }
}

void __free_pages(struct page *page, unsigned int order) {
  if (!page) return;

  if (PageBuddy(page)) {
    char buf[64];
    snprintf(buf, sizeof(buf), PMM_CLASS "Double free of page %p", page);
    panic(buf);
  }

  // PCP optimization for order-0
  if (order == 0 && percpu_ready()) {
    irq_flags_t flags = save_irq_flags();
    struct per_cpu_pages *pcp = this_cpu_ptr(pcp_pages);

    list_add(&page->list, &pcp->list);
    pcp->count++;

    if (pcp->count >= pcp->high) {
      struct pglist_data *pgdat = node_data[page->node];
      if (!pgdat) pgdat = node_data[0];

      struct zone *zone = &pgdat->node_zones[page->zone];
      struct list_head drain_list;
      INIT_LIST_HEAD(&drain_list);

      // Move 'batch' pages to a temporary list to free them under one lock
      for (int i = 0; i < pcp->batch; i++) {
        struct page *p = list_last_entry(&pcp->list, struct page, list);
        list_move(&p->list, &drain_list);
      }
      pcp->count -= pcp->batch;

      free_pcp_pages(zone, pcp->batch, &drain_list);
    }

    restore_irq_flags(flags);
    return;
  }

  unsigned long pfn = (unsigned long) (page - mem_map);
  struct pglist_data *pgdat = node_data[page->node];
  if (!pgdat) pgdat = node_data[0];

  struct zone *zone = &pgdat->node_zones[ZONE_NORMAL];

  // Determine zone.
  for (int i = 0; i < MAX_NR_ZONES; i++) {
    struct zone *z = &pgdat->node_zones[i];
    if (pfn >= z->zone_start_pfn && pfn < (z->zone_start_pfn + z->spanned_pages)) {
      zone = z;
      break;
    }
  }

  unsigned long flags;
  flags = spinlock_lock_irqsave(&zone->lock);
  __free_one_page(page, pfn, zone, order);
  spinlock_unlock_irqrestore(&zone->lock, flags);
}

void free_area_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;

    struct pglist_data *pgdat = node_data[n];
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      spinlock_init(&z->lock);
      z->name = zone_names[i];
      z->present_pages = 0;
      z->spanned_pages = 0;
      z->zone_start_pfn = 0;
      z->nr_free_pages = 0;
      z->max_free_order = 0;

      for (int order = 0; order < MAX_ORDER; order++) {
        INIT_LIST_HEAD(&z->free_area[order].free_list[0]);
        z->free_area[order].nr_free = 0;
      }
    }
  }
}
