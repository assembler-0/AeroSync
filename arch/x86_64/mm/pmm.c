/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/mm/pmm.c
 * @brief PMM for x86_64
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
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <arch/x86_64/features/features.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <compiler.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <limine/limine.h>
#include <linux/list.h>
#include <mm/gfp.h>
#include <mm/page.h>
#include <mm/zone.h>

// Global HHDM offset
uint64_t g_hhdm_offset = 0;
EXPORT_SYMBOL(g_hhdm_offset);

struct page *mem_map = nullptr;
uint64_t pmm_max_pages = 0;

bool pmm_initialized = false;
static pmm_stats_t pmm_stats;

// Find suitable memory region for mem_map array
static struct limine_memmap_entry *
find_memmap_location(struct limine_memmap_response *memmap,
                     uint64_t required_bytes) {
  struct limine_memmap_entry *best_region = nullptr;
  uint64_t best_size = 0;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type != LIMINE_MEMMAP_USABLE)
      continue;

    uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
    uint64_t aligned_end = PAGE_ALIGN_DOWN(entry->base + entry->length);
    if (aligned_end <= aligned_base)
      continue;

    uint64_t available = aligned_end - aligned_base;
    if (available >= required_bytes && available > best_size) {
      best_size = available;
      best_region = entry;
    }
  }
  return best_region;
}

extern void numa_init(void *rsdp);

extern int pfn_to_nid(uint64_t pfn);

uint64_t empty_zero_page = 0;
EXPORT_SYMBOL(empty_zero_page);

int pmm_init(void *memmap_response_ptr, uint64_t hhdm_offset, void *rsdp) {
  struct limine_memmap_response *memmap =
      (struct limine_memmap_response *)memmap_response_ptr;

  if (!memmap || memmap->entry_count == 0) {
    printk(KERN_ERR PMM_CLASS "Invalid memory map\n");
    return -EINVAL;
  }

  g_hhdm_offset = hhdm_offset;
  printk(KERN_DEBUG PMM_CLASS "Initializing buddy system...\n");

  // Initialize NUMA topology manually (early ACPI walk)
  numa_init(rsdp);

  // Pass 1: Calculate max PFN
  uint64_t highest_addr = 0;
  uint64_t total_usable_bytes = 0;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    uint64_t end = entry->base + entry->length;

    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
      if (end > highest_addr)
        highest_addr = end;
    }

    if (entry->type == LIMINE_MEMMAP_USABLE) {
      uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
      uint64_t aligned_end = PAGE_ALIGN_DOWN(end);
      if (aligned_end > aligned_base)
        total_usable_bytes += (aligned_end - aligned_base);
    }
  }

  pmm_max_pages = PHYS_TO_PFN(PAGE_ALIGN_UP(highest_addr));
  uint64_t memmap_size = pmm_max_pages * sizeof(struct page);

  // Allocate mem_map
  struct limine_memmap_entry *mm_region =
      find_memmap_location(memmap, PAGE_ALIGN_UP(memmap_size));
  if (!mm_region) {
    printk(KERN_ERR PMM_CLASS "Failed to allocate mem_map\n");
    return -ENOMEM;
  }

  uint64_t mm_phys = PAGE_ALIGN_UP(mm_region->base);
  mem_map = (struct page *)pmm_phys_to_virt(mm_phys);

  /*
   * Zero the entire mem_map in one shot.
   *
   * This is sufficient default initialization:
   *   - flags = 0:        Not reserved, not buddy, not slab.  Pages with
   *                        flags=0 that are never placed on a free list are
   *                        implicitly non-allocatable — the buddy allocator
   *                        only hands out pages from its free lists.
   *   - list.next/prev=0: Will be properly set by list_add() or
   *                        INIT_LIST_HEAD() when a page enters a free list.
   *   - order = 0, migratetype = 0 (MIGRATE_UNMOVABLE), node = 0, zone = 0,
   *     ptl = zeroed: All correct defaults.
   *
   * We intentionally do NOT walk every page struct to stamp PG_reserved.
   * The old code wasted ~1 second doing 1.3M individual stores.  Since
   * no code path returns an uninitialized page (the allocator only removes
   * from free list), the zero state is safe.
   */
  memset(mem_map, 0, memmap_size);

  // Initialize allocator zones
  free_area_init();

  // Set up Zones for each node with accurate boundaries
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;

    struct pglist_data *pgdat = node_data[n];

    // Fix for UMA fallback where numa_init might not know the max PFN yet
    if (n == 0 && pgdat->node_spanned_pages == 0xFFFFFFFF) {
      pgdat->node_spanned_pages = pmm_max_pages;
    }

    unsigned long node_start = pgdat->node_start_pfn;
    unsigned long node_end = node_start + pgdat->node_spanned_pages;

    // ZONE_DMA: [0, 16MB]
    pgdat->node_zones[ZONE_DMA].zone_start_pfn = node_start;
    if (node_start < 4096) {
      unsigned long end = node_end < 4096 ? node_end : 4096;
      pgdat->node_zones[ZONE_DMA].spanned_pages = end - node_start;
    } else {
      pgdat->node_zones[ZONE_DMA].spanned_pages = 0;
    }
    pgdat->node_zones[ZONE_DMA].present_pages = 0;

    // ZONE_DMA32: [16MB, 4GB]
    pgdat->node_zones[ZONE_DMA32].zone_start_pfn =
        node_start < 4096 ? 4096 : node_start;
    if (node_end > 4096 && node_start < 1048576) {
      unsigned long start = node_start < 4096 ? 4096 : node_start;
      unsigned long end = node_end < 1048576 ? node_end : 1048576;
      pgdat->node_zones[ZONE_DMA32].spanned_pages =
          (end > start) ? (end - start) : 0;
    } else {
      pgdat->node_zones[ZONE_DMA32].spanned_pages = 0;
    }
    pgdat->node_zones[ZONE_DMA32].present_pages = 0;

    // ZONE_NORMAL: [4GB, ...]
    pgdat->node_zones[ZONE_NORMAL].zone_start_pfn =
        node_start < 1048576 ? 1048576 : node_start;
    if (node_end > 1048576) {
      unsigned long start = node_start < 1048576 ? 1048576 : node_start;
      pgdat->node_zones[ZONE_NORMAL].spanned_pages = node_end - start;
    } else {
      pgdat->node_zones[ZONE_NORMAL].spanned_pages = 0;
    }
    pgdat->node_zones[ZONE_NORMAL].present_pages = 0;
  }

  /*
   * Pass 2: Feed free pages to the buddy allocator.
   *
   * For each usable memory region, find the largest naturally-aligned
   * power-of-2 block that fits, stamp the minimal per-page metadata
   * (node, zone), batch the present_pages accounting, and hand the
   * block to __free_pages_boot() which bypasses the PCP/deferred paths.
   */
  uint64_t mm_start_pfn = PHYS_TO_PFN(mm_phys);
  uint64_t mm_end_pfn = mm_start_pfn + (PAGE_ALIGN_UP(memmap_size) / PAGE_SIZE);

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type != LIMINE_MEMMAP_USABLE)
      continue;

    uint64_t start_pfn = PHYS_TO_PFN(PAGE_ALIGN_UP(entry->base));
    uint64_t end_pfn =
        PHYS_TO_PFN(PAGE_ALIGN_DOWN(entry->base + entry->length));

    // Exclude 0 page
    if (start_pfn == 0)
      start_pfn = 1;

    uint64_t cur_pfn = start_pfn;
    while (cur_pfn < end_pfn) {
      /* Jump past the mem_map region in O(1) instead of O(region_size). */
      if (cur_pfn >= mm_start_pfn && cur_pfn < mm_end_pfn) {
        cur_pfn = mm_end_pfn;
        continue;
      }

      // Find largest order that fits and is aligned
      unsigned int order = 0;
      uint64_t max_count = end_pfn - cur_pfn;

      // We also must stop before hitting the mem_map region
      if (cur_pfn < mm_start_pfn && (cur_pfn + max_count) > mm_start_pfn) {
        max_count = mm_start_pfn - cur_pfn;
      }

      for (unsigned int o = 0; o < MAX_ORDER - 1; o++) {
        uint64_t next_size = 1UL << (o + 1);
        if ((cur_pfn & (next_size - 1)) != 0)
          break;
        if (next_size > max_count)
          break;

        /* Check if the entire next_size block is on the same node and in the same zone */
        int start_nid = pfn_to_nid(cur_pfn);
        int end_nid = pfn_to_nid(cur_pfn + next_size - 1);
        if (start_nid != end_nid)
          break;

        int start_z = (cur_pfn < 4096) ? ZONE_DMA : (cur_pfn < 1048576 ? ZONE_DMA32 : ZONE_NORMAL);
        int end_z = ((cur_pfn + next_size - 1) < 4096) ? ZONE_DMA : ((cur_pfn + next_size - 1) < 1048576 ? ZONE_DMA32 : ZONE_NORMAL);
        if (start_z != end_z)
          break;

        order = o + 1;
      }

      int nid = pfn_to_nid(cur_pfn);
      struct pglist_data *pgdat = node_data[nid];
      if (!pgdat) {
        nid = 0;
        pgdat = node_data[0];
      }

      /*
       * Determine zone for the head PFN.  Blocks from the buddy's
       * order-finding loop above never cross zone boundaries because
       * zone thresholds (4096, 1048576) are power-of-2 aligned and
       * blocks are naturally aligned.  If a block did span a boundary,
       * the max_count limiter or alignment constraint would have
       * truncated it.  We can therefore set zone once per block.
       */
      int z_idx;
      if (cur_pfn < 4096)
        z_idx = ZONE_DMA;
      else if (cur_pfn < 1048576)
        z_idx = ZONE_DMA32;
      else
        z_idx = ZONE_NORMAL;

      uint64_t count = 1UL << order;

      /* Batch present_pages accounting — one add per block. */
      pgdat->node_zones[z_idx].present_pages += count;

      /*
       * Stamp minimal per-page metadata.
       * Only node and zone need to be set (migratetype is already 0 =
       * MIGRATE_UNMOVABLE from memset).  We deliberately write just
       * two uint32_t fields per page struct for cache efficiency.
       */
      for (uint64_t p = cur_pfn; p < cur_pfn + count; p++) {
        mem_map[p].node = nid;
        mem_map[p].zone = z_idx;
      }

      __free_pages_boot_core(&mem_map[cur_pfn], order);
      cur_pfn += count;
    }
  }

  pmm_initialized = true;

  build_all_zonelists();

  // Allocate and zero out global zero-page singleton
  empty_zero_page = pmm_alloc_page();
  if (empty_zero_page) {
    memset(pmm_phys_to_virt(empty_zero_page), 0, PAGE_SIZE);
    /* Set as Reserved so it's never freed or swapped */
    SetPageReserved(&mem_map[PHYS_TO_PFN(empty_zero_page)]);
  }

  // Calculate watermarks and log summary
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;
    struct pglist_data *pgdat = node_data[n];

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      if (z->present_pages > 0) {
        z->watermark[WMARK_MIN] = z->present_pages / 100;
        z->watermark[WMARK_LOW] = z->present_pages * 3 / 100;
        z->watermark[WMARK_HIGH] = z->present_pages * 5 / 100;
        z->watermark[WMARK_PROMO] = z->present_pages * 7 / 100;

#ifdef CONFIG_MM_PMM_HIGHATOMIC
        z->nr_reserved_highatomic =
            (CONFIG_MM_PMM_HIGHATOMIC_RESERVE_KB * 1024) / PAGE_SIZE;
        if (z->nr_reserved_highatomic > z->present_pages / 20)
          z->nr_reserved_highatomic = z->present_pages / 20;
#endif

        printk(KERN_DEBUG PMM_CLASS "node %d Zone %s: %lu pages\n", n, z->name,
               z->present_pages);
      }
    }
  }
  // Stats
  pmm_stats.total_pages = total_usable_bytes / PAGE_SIZE; // Approximate
  pmm_stats.highest_address = highest_addr;

  printk(KERN_DEBUG PMM_CLASS "Initialized. Max PFN: %llu\n", pmm_max_pages);

  pmm_report_capabilities();

  return 0;
}

void pmm_report_capabilities(void) {
  int active_nodes = 0;
  bool can_do_1g = false;
  uint64_t total_ram_mb = pmm_stats.total_pages * PAGE_SIZE / 1024 / 1024;

  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;
    active_nodes++;

    // Check if this node has an order 18 block
    struct pglist_data *pgdat = node_data[n];
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      if (pgdat->node_zones[i].max_free_order >= 18) {
        // Double check free_area[18]
        if (pgdat->node_zones[i].free_area[18].nr_free > 0) {
          can_do_1g = true;
          break;
        }
      }
    }
  }

  printk(KERN_INFO PMM_CLASS
         "system physical memory capabilities report (PMCR) ---\n");
  printk(KERN_INFO PMM_CLASS "Total Usable RAM: %llu MB\n", total_ram_mb);
  printk(KERN_INFO PMM_CLASS "NUMA Nodes: %d (Max supported: %d)\n",
         active_nodes, MAX_NUMNODES);

  if (can_do_1g) {
    printk(KERN_INFO PMM_CLASS "Contiguous 1GB Blocks: Available\n");
  } else {
    printk(KERN_WARNING PMM_CLASS "no 1GB contiguous memory block found "
                                  "(Memory too low or fragmented)\n");
    printk(KERN_WARNING PMM_CLASS
           "1GB hugepages will fail even though hardware supports them.\n");
  }

  if (total_ram_mb < 512) {
    printk(KERN_WARNING PMM_CLASS
           "Low Memory Warning: System has less than 512MB RAM.\n");
    printk(KERN_WARNING PMM_CLASS
           "Performance may be degraded and large allocations will fail.\n");
  }
}

void pmm_test(void) {
  // Smoke Test
  printk(KERN_DEBUG PMM_CLASS "Running smoke test...\n");

  // Test 1: Single page allocation
  uint64_t p1 = pmm_alloc_page();
  if (!p1) {
    printk(KERN_ERR PMM_CLASS "Smoke test failed (alloc 1)\n");
  } else {
    uint64_t *v1 = (uint64_t *)pmm_phys_to_virt(p1);
    *v1 = 0xDEADBEEFCAFEBABE;
    if (*v1 != 0xDEADBEEFCAFEBABE) {
      printk(KERN_ERR PMM_CLASS "Smoke test failed (read/write 1)\n");
    } else {
      printk(KERN_DEBUG PMM_CLASS "Alloc 1 OK (phys: 0x%llx)\n", p1);
    }
    pmm_free_page(p1);
  }

  // Test 2: Multi-page allocation (Order 2 -> 4 pages)
  uint64_t p2 = pmm_alloc_pages(4);
  if (!p2) {
    printk(KERN_ERR PMM_CLASS "Smoke test failed (alloc 4)\n");
  } else {
    // Verify alignment (Order 2 requires 16KB alignment usually, but buddy
    // guarantees natural alignment of block)
    if (p2 & (PAGE_SIZE * 4 - 1)) {
      printk(KERN_WARNING PMM_CLASS
             "Alloc 4 alignment check warning (0x%llx)\n",
             p2);
    }

    uint64_t *v2 = (uint64_t *)pmm_phys_to_virt(p2);
    v2[0] = 0xAAAAAAAA;
    v2[512 * 3] = 0xBBBBBBBB; // Write to 4th page

    if (v2[0] != 0xAAAAAAAA || v2[512 * 3] != 0xBBBBBBBB) {
      printk(KERN_ERR PMM_CLASS "Smoke test failed (read/write 4)\n");
    } else {
      printk(KERN_DEBUG PMM_CLASS "Alloc 4 OK (phys: 0x%llx)\n", p2);
    }
    pmm_free_pages(p2, 4);
  }

  printk(KERN_DEBUG PMM_CLASS "Smoke test complete.\n");
}

uint64_t pmm_alloc_page(void) { return pmm_alloc_pages(1); }

uint64_t pmm_alloc_huge(size_t size) {
  if (!pmm_initialized || !vmm_page_size_supported(size))
    return 0;

  unsigned int order = 0;
  if (size > PAGE_SIZE) {
    order = 64 - __builtin_clzll((size >> PAGE_SHIFT) - 1);
  }

  struct folio *folio = alloc_pages(GFP_KERNEL, order);
  if (!folio)
    return 0;

  return folio_to_phys(folio);
}

uint64_t pmm_alloc_pages(size_t count) {
  if (!pmm_initialized)
    return 0;

  // Calculate order
  unsigned int order = 0;
  if (count > 1) {
    order = 64 - __builtin_clzll(count - 1);
  }

  struct folio *folio = alloc_pages(GFP_KERNEL, order);
  if (!folio)
    return 0;

  return folio_to_phys(folio);
}

void pmm_free_page(uint64_t phys_addr) { pmm_free_pages(phys_addr, 1); }

void pmm_free_pages(uint64_t phys_addr, size_t count) {
  if (!pmm_initialized)
    return;

  uint64_t pfn = PHYS_TO_PFN(phys_addr);
  struct page *page = &mem_map[pfn];

  if (count > 0 && count != (1UL << page->order)) {
    printk(
        KERN_WARNING PMM_CLASS
        "pmm_free_pages: count %zu does not match page order %u (pfn %llu)\n",
        count, page->order, pfn);
  }

  put_page(page);
}

uint64_t pmm_get_max_pfn(void) { return pmm_max_pages; }

EXPORT_SYMBOL(pmm_get_max_pfn);

void pmm_init_cpu(void) {
  int cpu = (int)smp_get_id();
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n])
      continue;
    struct pglist_data *pgdat = node_data[n];
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      struct per_cpu_pages *pcp = &z->pageset[cpu];

      for (int o = 0; o < PCP_ORDERS; o++) {
#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
        INIT_LIST_HEAD(&pcp->lists[o][PCP_LIST_HOT]);
        INIT_LIST_HEAD(&pcp->lists[o][PCP_LIST_COLD]);
#else
        INIT_LIST_HEAD(&pcp->lists[o][0]);
#endif
      }

      pcp->count = 0;
      pcp->high = 64;
      pcp->batch = 16;

#ifdef CONFIG_MM_PMM_PCP_DYNAMIC
      pcp->high_min = 32;
      pcp->high_max = 256;
      pcp->batch_min = 8;
      pcp->batch_max = 64;
#endif

#ifdef CONFIG_MM_PMM_STATS
      atomic_long_set(&pcp->alloc_count, 0);
      atomic_long_set(&pcp->free_count, 0);
      atomic_long_set(&pcp->refill_count, 0);
      atomic_long_set(&pcp->drain_count, 0);
#endif
    }
  }
}

pmm_stats_t *pmm_get_stats(void) { return &pmm_stats; }

EXPORT_SYMBOL(pmm_virt_to_phys);
EXPORT_SYMBOL(pmm_phys_to_virt);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
EXPORT_SYMBOL(pmm_alloc_pages);
EXPORT_SYMBOL(pmm_free_pages);
