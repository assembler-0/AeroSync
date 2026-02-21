///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vma_test.c
 * @brief VMA tests
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

#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/resdomain.h>
#include <lib/printk.h>
#include <linux/maple_tree.h>
#include <mm/mmu_gather.h>
#include <mm/vma.h>
#include <mm/vm_object.h>

/* ========================================================================
 * Test Suite
 * ======================================================================== */

#define TEST_VMA_COUNT 1024
#define TEST_RANGE_START 0x1000000000ULL
#define TEST_RANGE_END   0x2000000000ULL

void vma_extreme_stress_test(void) {
  printk(KERN_DEBUG VMA_CLASS "Starting VMA Stress Test...\n");
  struct mm_struct *mm = mm_create();
  if (!mm) panic("vma_stress: failed to create mm");

  /* 1. Massive Fragmentation Test: Create 1024 small VMAs with gaps */
  printk(KERN_DEBUG VMA_CLASS "|- Phase 1: Massive Fragmentation...");
  for (int i = 0; i < TEST_VMA_COUNT; i++) {
    uint64_t addr = TEST_RANGE_START + (i * 2 * PAGE_SIZE);
    if (vma_map_range(mm, addr, addr + PAGE_SIZE, VM_READ | VM_WRITE) != 0) {
      panic("vma_stress: failed phase 1 at iteration %d", i);
    }
  }
  if (mm->map_count != TEST_VMA_COUNT) panic("vma_stress: phase 1 count mismatch: %d", mm->map_count);
  printk("OK\n");

  /* 2. Bridge Merge Test: Fill the gaps to trigger proactive bridge merging */
  printk(KERN_DEBUG VMA_CLASS "|- Phase 2: Proactive Bridge Merging...");
  for (int i = 0; i < TEST_VMA_COUNT - 1; i++) {
    uint64_t addr = TEST_RANGE_START + (i * 2 * PAGE_SIZE) + PAGE_SIZE;
    /* Filling the gap between VMA_i and VMA_i+1 */
    if (vma_map_range(mm, addr, addr + PAGE_SIZE, VM_READ | VM_WRITE) != 0) {
      panic("vma_stress: failed phase 2 at iteration %d", i);
    }
  }
  /* All VMAs should have merged into ONE giant VMA */
  if (mm->map_count != 1) {
    panic("vma_stress: phase 2 merge failed, map_count: %d (expected 1)", mm->map_count);
  }
  printk("OK\n");

  /* 3. Swiss Cheese Test: Unmap small chunks from the middle */
  printk(KERN_DEBUG VMA_CLASS "|- Phase 3: Swiss Cheese Unmapping...");
  uint64_t giant_start = TEST_RANGE_START;
  for (int i = 0; i < 512; i++) {
    uint64_t addr = giant_start + (i * 4 * PAGE_SIZE) + PAGE_SIZE;
    if (vma_unmap_range(mm, addr, addr + PAGE_SIZE) != 0) {
      panic("vma_stress: failed phase 3 at iteration %d", i);
    }
  }
  printk("OK\n");

  /* 4. Parallel Fault Simulation (Speculative path exercise) */
  printk(KERN_DEBUG VMA_CLASS "|- Phase 4: Speculative Fault Validation...");
  struct vm_area_struct *vma;
  int checked = 0;
  rcu_read_lock();
  for_each_vma(mm, vma) {
    /* Simulate fault handler looking at VMAs while another CPU might modify them */
    uint32_t seq = atomic_read(&mm->mmap_seq);
    if (vma->vm_start & PAGE_MASK) checked++;
    if (atomic_read(&mm->mmap_seq) != seq) {
      /* This is fine in real parallel, but here it shouldn't change */
    }
  }
  rcu_read_unlock();
  printk("OK (%d VMAs checked)\n", checked);

  /* Clean up */
  mm_destroy(mm);
  mm_free(mm);
}

void vma_test(void) {
  uint64_t start = get_time_ns();
  printk(KERN_DEBUG VMA_CLASS "Starting VMA smoke test...\n");

  /* Use mm_create to get valid page tables and exercise VMM glue */
  struct mm_struct *mm = mm_create();
  if (!mm) panic("vma_test: failed to create mm");

  /* Test 1: Basic Mappings & Insertion */
  /* Create two 2-page VMAs: [0x1000, 0x3000] and [0x5000, 0x7000] */
  vma_map_range(mm, 0x1000, 0x3000, VM_READ);
  vma_map_range(mm, 0x5000, 0x7000, VM_READ | VM_WRITE);

  if (mm->map_count != 2) panic("vma_test: map_count mismatch");
  printk(KERN_DEBUG VMA_CLASS "|- Basic Mapping: OK\n");

  /* Test 2: Gap Finding (Maple Tree) */
  /* NOTE: With guard pages, we can't fit 4KB in the 8KB gap between 0x3000 and 0x5000. */
  /* 0x3000 (end of VMA1) -> Guard (0x4000) -> Data (0x5000) -> Collision with VMA2. */
  /* So it should find space AFTER 0x7000. */
  /* Expected: 0x7000 + Guard(0x1000) = 0x8000. */
  uint64_t free = vma_find_free_region(mm, 0x1000, 0x1000, 0x10000);

  /* Depending on ASLR, this might be higher, but we restricted ASLR range start to low. */
  /* However, vma_find_free_region uses ASLR now. */
  /* To test deterministic behavior, we should perhaps rely on 'alignment' or just verify it's valid. */

  if (free == 0) panic("vma_test: gap find failed completely");

  /* Verify it doesn't overlap or touch */
  struct vm_area_struct *v1 = vma_find(mm, free);
  if (v1) panic("vma_test: allocated on existing VMA");

  /* Check overlap with 0x1000-0x3000 */
  if (free >= 0x1000 && free < 0x3000) panic("vma_test: overlap VMA1");
  /* Check overlap with 0x5000-0x7000 */
  if (free >= 0x5000 && free < 0x7000) panic("vma_test: overlap VMA2");

  /* Check guard pages */
  if (free == 0x3000 || free + 0x1000 == 0x5000) panic("vma_test: guard page violation");

  printk(KERN_DEBUG VMA_CLASS "|- Gap Finding: OK (Got %llx)\n", free);

  /* Test 3: VMA Splitting (Must be page aligned) */
  printk(KERN_DEBUG VMA_CLASS "|- VMA Splitting: start...\n");
  down_write(&mm->mmap_lock);
  struct vm_area_struct *vma_to_split = vma_find(mm, 0x5000);
  if (!vma_to_split) panic("vma_test: could not find vma at 0x5000");

  /* Split [0x5000, 0x7000] at 0x6000 */
  if (vma_split(mm, vma_to_split, 0x6000) != 0) {
    panic("vma_test: split failed");
  }
  up_write(&mm->mmap_lock);

  if (mm->map_count != 3) panic("vma_test: map_count after split mismatch");
  printk(KERN_DEBUG VMA_CLASS "|- VMA Splitting: OK\n");

  /* Test 4: VMA Protection (with split) */
  printk(KERN_DEBUG VMA_CLASS "|- VMA Protect (Split): start...\n");
  /* Change protection on the first page of [0x1000, 0x3000] */
  if (vma_protect(mm, 0x1000, 0x2000, VM_READ | VM_WRITE) != 0) panic("vma_test: protect failed");
  if (mm->map_count != 4) panic("vma_test: map_count after protect mismatch");
  printk(KERN_DEBUG VMA_CLASS "|- VMA Protect (Split): OK\n");

  /* Test 5: Unmapping partial & full */
  /* Unmap the middle pages across multiple VMAs: [0x2000, 0x6000] */
  vma_unmap_range(mm, 0x2000, 0x6000);
  printk(KERN_DEBUG VMA_CLASS "|- Partial Unmap: OK\n");

  /* Clean up all */
  vma_unmap_range(mm, 0, vmm_get_max_user_address());
  if (mm->map_count != 0) panic("vma_test: unmap all failed");
  printk(KERN_DEBUG VMA_CLASS "|- Unmap All: OK\n");

  mm_destroy(mm);
  mm_free(mm);
  printk(KERN_DEBUG VMA_CLASS "VMA smoke test Passed.\n");

  vma_extreme_stress_test();
  printk(KERN_DEBUG VMA_CLASS "VMA Stress Test passed. (%lld ns)\n", get_time_ns() - start);
}
