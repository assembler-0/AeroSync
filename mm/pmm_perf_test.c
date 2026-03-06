/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/pmm_perf_test.c
 * @brief PMM performance tests (Latency verification for kzeropaged)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/tsc.h>
#include <lib/printk.h>
#include <mm/gfp.h>
#include <mm/page.h>
#include <mm/zone.h>
#include <aerosync/classes.h>

#define ITERATIONS 1000
#define BATCH_SIZE 1

void pmm_perform_zero_pool_test(void) {
  printk(KERN_INFO PMM_CLASS "Starting Zero-Pool Performance Verification...\n");
  
  struct page *pages[BATCH_SIZE];
  uint64_t start, end;
  uint64_t total_cycles_raw = 0;
  uint64_t total_cycles_zero_pool = 0;
  uint64_t total_cycles_memset = 0;

  /* 1. Measure raw allocation (no zeroing) */
  for (int i = 0; i < ITERATIONS; i++) {
    start = rdtsc();
    unsigned long n = alloc_pages_bulk_array(0, GFP_KERNEL, 0, BATCH_SIZE, pages);
    end = rdtsc();
    if (n == BATCH_SIZE) {
      total_cycles_raw += (end - start);
      free_pages_bulk_array(BATCH_SIZE, pages);
    }
  }

  /* 2. Measure __GFP_ZERO with pool (hopefully) full */
  /* Actually, we just run it and see if we hit the pool */
  for (int i = 0; i < ITERATIONS; i++) {
    start = rdtsc();
    unsigned long n = alloc_pages_bulk_array(0, GFP_KERNEL | __GFP_ZERO, 0, BATCH_SIZE, pages);
    end = rdtsc();
    if (n == BATCH_SIZE) {
      total_cycles_zero_pool += (end - start);
      free_pages_bulk_array(BATCH_SIZE, pages);
    }
  }

  /* 3. Measure __GFP_ZERO while bypassing pool (to simulate pool exhaustion/cold path) */
  /* We drain it and let kzeropaged refill it later */
  struct zone *z = &node_data[0]->node_zones[ZONE_NORMAL];
  if (!z->present_pages) z = &node_data[0]->node_zones[ZONE_DMA32];
  
  struct list_head temp_list;
  INIT_LIST_HEAD(&temp_list);
  
  /* Drain the pool */
  irq_flags_t flags = spinlock_lock_irqsave(&z->lock);
  list_cut_position(&temp_list, &z->zero_list, z->zero_list.prev);
  z->nr_zero_pages = 0;
  spinlock_unlock_irqrestore(&z->lock, flags);

  for (int i = 0; i < ITERATIONS; i++) {
    start = rdtsc();
    unsigned long n = alloc_pages_bulk_array(0, GFP_KERNEL | __GFP_ZERO, 0, BATCH_SIZE, pages);
    end = rdtsc();
    if (n == BATCH_SIZE) {
      total_cycles_memset += (end - start);
      free_pages_bulk_array(BATCH_SIZE, pages);
    }
  }

  /* 4. Free the drained pages back to buddy (NOT back to zero_list) */
  struct page *p, *tmp;
  list_for_each_entry_safe(p, tmp, &temp_list, list) {
    list_del(&p->list);
    __free_page(p);
  }

  uint64_t avg_raw = total_cycles_raw / ITERATIONS;
  uint64_t avg_pool = total_cycles_zero_pool / ITERATIONS;
  uint64_t avg_memset = total_cycles_memset / ITERATIONS;

  printk(KERN_INFO PMM_CLASS "--- PMM Latency Results (Average TSC Cycles) ---\n");
  printk(KERN_INFO PMM_CLASS "Raw Alloc (No Zeroing): %llu cycles\n", avg_raw);
  printk(KERN_INFO PMM_CLASS "Zero Pool Hit:          %llu cycles\n", avg_pool);
  printk(KERN_INFO PMM_CLASS "Zero Pool Miss (Memset): %llu cycles\n", avg_memset);
  
  if (avg_pool < avg_memset && avg_memset > 0) {
    uint64_t gain = (avg_memset - avg_pool) * 100 / avg_memset;
    printk(KERN_INFO PMM_CLASS "Pre-zeroing Speedup:    %llu%%\n", gain);
  } else {
    printk(KERN_WARNING PMM_CLASS "Pre-zeroing Gain:       None (Check kzeropaged state)\n");
  }
  printk(KERN_INFO PMM_CLASS "-----------------------------------------------\n");
}
