/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/mm/pmm.c
 * @brief PMM for x86_64
 * @copyright (C) 2025 assembler-0
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

#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/percpu.h>
#include <compiler.h>
#include <arch/x86_64/features/features.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <limine/limine.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <mm/gfp.h>
#include <mm/zone.h>

// Global HHDM offset
uint64_t g_hhdm_offset = 0;
EXPORT_SYMBOL(g_hhdm_offset);

DEFINE_PER_CPU(struct per_cpu_pages, pcp_pages);

struct page *mem_map = NULL;
uint64_t pmm_max_pages = 0;

static bool pmm_initialized = false;
static pmm_stats_t pmm_stats;

// Find suitable memory region for mem_map array
static struct limine_memmap_entry *
find_memmap_location(struct limine_memmap_response *memmap,
                     uint64_t required_bytes) {
  struct limine_memmap_entry *best_region = NULL;
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

int pmm_init(void *memmap_response_ptr, uint64_t hhdm_offset, void *rsdp) {
  struct limine_memmap_response *memmap =
      (struct limine_memmap_response *) memmap_response_ptr;

  if (!memmap || memmap->entry_count == 0) {
    printk(KERN_ERR PMM_CLASS "Invalid memory map\n");
    return -1;
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
  mem_map = (struct page *) pmm_phys_to_virt(mm_phys);
  memset(mem_map, 0, memmap_size);

  // Init all pages as Reserved
  for (uint64_t i = 0; i < pmm_max_pages; i++) {
    INIT_LIST_HEAD(&mem_map[i].list);
    mem_map[i].flags = PG_reserved;
    mem_map[i].order = 0;
    mem_map[i].migratetype = MIGRATE_UNMOVABLE;
    mem_map[i].node = 0; // Default
    spinlock_init(&mem_map[i].ptl);
  }

  // Initialize allocator zones
  free_area_init();

  // Set up Zones for each node with accurate boundaries
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;

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
    pgdat->node_zones[ZONE_DMA32].zone_start_pfn = node_start < 4096 ? 4096 : node_start;
    if (node_end > 4096 && node_start < 1048576) {
      unsigned long start = node_start < 4096 ? 4096 : node_start;
      unsigned long end = node_end < 1048576 ? node_end : 1048576;
      pgdat->node_zones[ZONE_DMA32].spanned_pages = (end > start) ? (end - start) : 0;
    } else {
      pgdat->node_zones[ZONE_DMA32].spanned_pages = 0;
    }
    pgdat->node_zones[ZONE_DMA32].present_pages = 0;

    // ZONE_NORMAL: [4GB, ...]
    pgdat->node_zones[ZONE_NORMAL].zone_start_pfn = node_start < 1048576 ? 1048576 : node_start;
    if (node_end > 1048576) {
      unsigned long start = node_start < 1048576 ? 1048576 : node_start;
      pgdat->node_zones[ZONE_NORMAL].spanned_pages = node_end - start;
    } else {
      pgdat->node_zones[ZONE_NORMAL].spanned_pages = 0;
    }
    pgdat->node_zones[ZONE_NORMAL].present_pages = 0;
  }

  // Pass 2: Feed free pages to allocator
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
      if (cur_pfn >= mm_start_pfn && cur_pfn < mm_end_pfn) {
        cur_pfn++;
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
        if ((cur_pfn & (1UL << (o + 1))) != 0) break;
        if ((1UL << (o + 1)) > max_count) break;
        order = o + 1;
      }

      int nid = pfn_to_nid(cur_pfn);
      struct pglist_data *pgdat = node_data[nid];
      if (!pgdat) {
        nid = 0;
        pgdat = node_data[0];
      }

      // Register pages in the block
      uint64_t count = 1UL << order;
      for (uint64_t i = 0; i < count; i++) {
        struct page *page = &mem_map[cur_pfn + i];
        ClearPageReserved(page);
        page->node = nid;
        page->migratetype = MIGRATE_UNMOVABLE;

        int z_idx = 0;
        if ((cur_pfn + i) < 4096) z_idx = ZONE_DMA;
        else if ((cur_pfn + i) < 1048576) z_idx = ZONE_DMA32;
        else z_idx = ZONE_NORMAL;

        page->zone = z_idx;
        pgdat->node_zones[z_idx].present_pages++;
      }

      __free_pages(&mem_map[cur_pfn], order);
      cur_pfn += count;
    }
  }

  pmm_initialized = true;

  // Calculate watermarks and log summary
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;
    struct pglist_data *pgdat = node_data[n];

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      if (z->present_pages > 0) {
        z->watermark[WMARK_MIN] = z->present_pages / 100;
        z->watermark[WMARK_LOW] = z->present_pages * 3 / 100;
        z->watermark[WMARK_HIGH] = z->present_pages * 5 / 100;

        printk(KERN_DEBUG PMM_CLASS "node %d Zone %s: %lu pages\n",
               n, z->name, z->present_pages);
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
    if (!node_data[n]) continue;
    active_nodes++;

    // Check if this node has an order 18 block
    struct pglist_data *pgdat = node_data[n];
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      if (pgdat->node_zones[i].free_area[18].nr_free > 0) {
        can_do_1g = true;
        break;
      }
    }
  }

  printk(KERN_INFO PMM_CLASS "system physical memory capabilities report (PMCR) ---\n");
  printk(KERN_INFO PMM_CLASS "Total Usable RAM: %llu MB\n", total_ram_mb);
  printk(KERN_INFO PMM_CLASS "NUMA Nodes: %d (Max supported: %d)\n", active_nodes, MAX_NUMNODES);

  if (can_do_1g) {
    printk(KERN_INFO PMM_CLASS "Contiguous 1GB Blocks: Available\n");
  } else {
    printk(KERN_WARNING PMM_CLASS "no 1GB contiguous memory block found (Memory too low or fragmented)\n");
    printk(KERN_WARNING PMM_CLASS "1GB hugepages will fail even though hardware supports them.\n");
  }


  if (total_ram_mb < 512) {
    printk(KERN_WARNING PMM_CLASS "Low Memory Warning: System has less than 512MB RAM.\n");
    printk(KERN_WARNING PMM_CLASS "Performance may be degraded and large allocations will fail.\n");
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
    uint64_t *v1 = (uint64_t *) pmm_phys_to_virt(p1);
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

    uint64_t *v2 = (uint64_t *) pmm_phys_to_virt(p2);
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
  if (!folio) return 0;

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
    printk(KERN_WARNING PMM_CLASS "pmm_free_pages: count %zu does not match page order %u (pfn %llu)\n",
           count, page->order, pfn);
  }

  put_page(page);
}

void pmm_init_cpu(void) {
  struct per_cpu_pages *pcp = this_cpu_ptr(pcp_pages);
  INIT_LIST_HEAD(&pcp->list);
  pcp->count = 0;
  pcp->high = 32;
  pcp->batch = 8;
}

pmm_stats_t *pmm_get_stats(void) { return &pmm_stats; }

EXPORT_SYMBOL(pmm_virt_to_phys);
EXPORT_SYMBOL(pmm_phys_to_virt);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
EXPORT_SYMBOL(pmm_alloc_pages);
EXPORT_SYMBOL(pmm_free_pages);
