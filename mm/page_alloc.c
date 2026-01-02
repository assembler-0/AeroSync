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
#include <arch/x64/cpu.h>
#include <kernel/classes.h>
#include <linux/list.h>
#include <linux/container_of.h>
#include <mm/gfp.h>
#include <kernel/sched/sched.h>

/* Global zones */
struct zone managed_zones[MAX_NR_ZONES];

/* Default zone names */
static const char *const zone_names[MAX_NR_ZONES] = {
  "DMA",
  "DMA32",
  "Normal"
};

/* Defined in arch/x64/mm/pmm.c */
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

    expand(zone, page, order, current_order, area);
    return page;
  }

  return NULL;
}

static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order) {
  unsigned long buddy_pfn;
  struct page *buddy;

  while (order < MAX_ORDER - 1) {
    buddy_pfn = __find_buddy_pfn(pfn, order);

    if (buddy_pfn >= zone->zone_start_pfn + zone->spanned_pages)
      break;

    // Check if buddy is within max pages (global check)
    if (buddy_pfn >= pmm_max_pages)
      break;

    buddy = &mem_map[buddy_pfn];

    if (!page_is_buddy(page, buddy, order))
      break;

    /* Our buddy is free, merge with it */
    list_del(&buddy->list);
    zone->free_area[order].nr_free--;
    zone->nr_free_pages -= (1UL << order);
    ClearPageBuddy(buddy);

    pfn &= ~(1UL << order);
    page = &mem_map[pfn];
    order++;
  }

  SetPageBuddy(page);
  page->order = order;
  list_add(&page->list, &zone->free_area[order].free_list[0]);
  zone->free_area[order].nr_free++;
  zone->nr_free_pages += (1UL << order);
}

/*
 * Core Allocator
 */
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order) {
  struct page *page = NULL;
  struct zone *z;
  int z_idx;
  unsigned long flags;

  if (nid < 0 || nid >= MAX_NUMNODES || !node_data[nid]) {
      nid = 0; // Fallback to node 0
  }

  int start_zone = ZONE_NORMAL;
  if (gfp_mask & ___GFP_DMA) start_zone = ZONE_DMA;
  else if (gfp_mask & ___GFP_DMA32) start_zone = ZONE_DMA32;

  /* 
   * Try preferred node first
   */
  struct pglist_data *pgdat = node_data[nid];
  for (z_idx = start_zone; z_idx >= 0; z_idx--) {
    z = &pgdat->node_zones[z_idx];

    if (!z->present_pages) continue;

    /* Check watermarks */
    if (z->nr_free_pages < z->watermark[WMARK_LOW]) {
        wakeup_kswapd(z);
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
        if (!z->present_pages) continue;

        flags = spinlock_lock_irqsave(&z->lock);
        page = __rmqueue(z, order);
        spinlock_unlock_irqrestore(&z->lock, flags);

        if (page) goto found;
      }
  }

  printk(KERN_ERR PMM_CLASS "failed to allocate order %u from any node\n", order);
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
  return (struct folio *)page;
}

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order) {
    int nid = 0;
    struct task_struct *curr = get_current();
    if (curr) nid = curr->node_id;
    
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

      for (int order = 0; order < MAX_ORDER; order++) {
        INIT_LIST_HEAD(&z->free_area[order].free_list[0]);
        z->free_area[order].nr_free = 0;
      }
    }
  }
}