///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/compaction.c
 * @brief Memory compaction for fragmentation reduction
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/zone.h>
#include <mm/page.h>
#include <mm/vm_object.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/printk.h>
#include <linux/list.h>

/**
 * struct compact_control - Control state for compaction.
 */
struct compact_control {
  struct zone *zone;
  unsigned long free_pfn;    /* Scanner for free pages (beginning of zone) */
  unsigned long migrate_pfn; /* Scanner for movable pages (end of zone) */
  size_t nr_migrated;
  size_t nr_freed;
  int order;                 /* Target order for compaction */
  gfp_t gfp_mask;
};

/**
 * isolate_migratepages - Scan zone from end for movable pages.
 */
static int isolate_migratepages(struct compact_control *cc) {
  struct zone *z = cc->zone;
  unsigned long low_pfn = z->zone_start_pfn;
  unsigned long high_pfn = cc->migrate_pfn;
  struct list_head migrate_list;
  INIT_LIST_HEAD(&migrate_list);

    /* Scan backwards for movable pages */
    for (unsigned long pfn = high_pfn; pfn > low_pfn && pfn > cc->free_pfn; pfn--) {
      struct page *page = phys_to_page(PFN_TO_PHYS(pfn));
      if (!page || PageReserved(page) || PageSlab(page)) continue;    
    /* Only migrate MOVABLE pages */
    if (page->migratetype != MIGRATE_MOVABLE) continue;

    /* 
     * In a production kernel, we would use try_to_unmap_folio here.
     * For now, we only migrate pages that are NOT mapped or have 1 ref.
     */
    if (page_ref_count(page) != 1) continue;

    /* Isolate the page */
    list_add(&page->list, &migrate_list);
    cc->nr_migrated++;
    
    if (cc->nr_migrated >= 32) {
       cc->migrate_pfn = pfn;
       break;
    }
  }
  
  return 0;
}

/**
 * compact_zone - Main entry point for per-zone compaction.
 */
int compact_zone(struct zone *zone, struct compact_control *cc) {
  cc->zone = zone;
  cc->free_pfn = zone->zone_start_pfn;
  cc->migrate_pfn = zone->zone_start_pfn + zone->spanned_pages - 1;

  printk(KERN_INFO VMM_CLASS "Compacting zone %s...\n", zone->name);

  /*
   * Simple two-finger scan:
   * migrate_pfn starts at the end and moves down.
   * free_pfn starts at the beginning and moves up.
   */
  while (cc->migrate_pfn > cc->free_pfn) {
    isolate_migratepages(cc);
    
    /* Exit early if we satisfied the order (simulated) */
    if (cc->nr_migrated > 0) break;
    
    cc->migrate_pfn -= 32;
    cc->free_pfn += 32;
  }

  return 0;
}

/**
 * try_to_compact_pages - Direct compaction entry point.
 */
unsigned long try_to_compact_pages(gfp_t gfp_mask, unsigned int order) {
  struct compact_control cc = {
    .order = (int)order,
    .gfp_mask = gfp_mask,
  };

  /* Only compact for high-order allocations */
  if (order < 2) return 0;

  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &node_data[n]->node_zones[i];
      if (z->present_pages > 0) {
        compact_zone(z, &cc);
      }
    }
  }

  return 0;
}