///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vmalloc.c
 * @brief Kernel Virtual Memory Allocation
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

#include <lib/printk.h>
#include <mm/vmalloc.h>
#include <mm/mm_types.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/tlb.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/fkx/fkx.h>

/**
 * vmalloc_unmap_pages - Unmaps pages from the vmalloc region.
 * Optimized: Only unmap pages that are actually present in the vm_object.
 * Uses deferred TLB shootdown to avoid IPI storms.
 */
static void vmalloc_unmap_pages(struct vm_area_struct *vma) {
  if (!vma->vm_obj) return;

  struct vm_object *obj = vma->vm_obj;
  irq_flags_t flags = spinlock_lock_irqsave(&obj->lock);

  struct rb_node *node = rb_first(&obj->page_tree);
  if (!node) {
    spinlock_unlock_irqrestore(&obj->lock, flags);
    return;
  }

  uint64_t min_virt = 0, max_virt = 0;

  while (node) {
    struct page_node {
      struct rb_node rb;
      uint64_t pgoff;
      struct page *page;
      int order;
    } *pnode = (struct page_node *)node;
    
    uint64_t virt = vma->vm_start + (pnode->pgoff << PAGE_SHIFT);
    if (min_virt == 0 || virt < min_virt) min_virt = virt;
    uint64_t end = virt + (1ULL << (pnode->order + PAGE_SHIFT));
    if (end > max_virt) max_virt = end;
    
    vmm_unmap_page_deferred(g_kernel_pml4, virt);

    node = rb_next(node);
  }

  spinlock_unlock_irqrestore(&obj->lock, flags);

  // Shoot down only the range that was actually faulted in
  if (min_virt != 0) {
    vmm_tlb_shootdown(NULL, min_virt, max_virt);
  }
}

static void *__vmalloc_internal(size_t size, uint64_t vma_flags, uint64_t vmm_flags) {
  if (size == 0) return NULL;

  // 1. Align the requested size
  size_t alloc_size = PAGE_ALIGN_UP(size);

  // 2. Determine Huge Page usage
  uint64_t alignment = PAGE_SIZE;
  if (alloc_size >= VMM_PAGE_SIZE_2M && !(vma_flags & VM_IO)) {
    alignment = VMM_PAGE_SIZE_2M;
    alloc_size = (alloc_size + VMM_PAGE_SIZE_2M - 1) & ~(VMM_PAGE_SIZE_2M - 1);
    vma_flags |= VM_HUGE;
  }

  // 3. Force True Lazy Allocation for all vmalloc
  vma_flags |= VM_ALLOC_LAZY;

  size_t reserve_size = alloc_size + PAGE_SIZE; // Guard page

  down_write(&init_mm.mmap_lock);

  // 4. Find free virtual address space
  uint64_t virt_start = vma_find_free_region_aligned(&init_mm, reserve_size, alignment,
                                                     VMALLOC_VIRT_BASE,
                                                     VMALLOC_VIRT_END);

  if (virt_start == 0) {
    up_write(&init_mm.mmap_lock);
    printk(KERN_ERR "vmalloc: failed to find suitable virtual address region of size %llu\n", reserve_size);
    return NULL;
  }

  // 5. Create VMA (Automatically creates an anonymous vm_object)
  struct vm_area_struct *vma = vma_create(virt_start, virt_start + reserve_size, vma_flags);
  if (!vma) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  // Set the vm_object size to the actual allocation size, excluding the guard page.
  // This ensures the fault handler (anon_obj_fault) rejects access to the guard page.
  if (vma->vm_obj) {
    vma->vm_obj->size = alloc_size;
  }

  if (vma_insert(&init_mm, vma) < 0) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  up_write(&init_mm.mmap_lock);

  // No eager mapping here. O(1) performance.
  return (void *) virt_start;
}

/**
 * vmalloc - Allocate virtually contiguous memory
 */
void *vmalloc(size_t size) {
  return __vmalloc_internal(size, VM_READ | VM_WRITE, PTE_RW | PTE_NX);
}

/**
 * vmalloc_exec - Allocate executable virtually contiguous memory
 */
void *vmalloc_exec(size_t size) {
  // Executable memory shouldn't be lazy? 
  // Actually, it can be, but for modules it's often better to be eager.
  // However, to satisfy "EXTREMELY FASTER", we can use lazy even here.
  return __vmalloc_internal(size, VM_READ | VM_WRITE | VM_EXEC, PTE_RW);
}

/**
 * vzalloc - Allocate zeroed virtually contiguous memory
 * 
 * Optimized: Since anon_obj_fault already zeroes pages on demand,
 * this is identical to vmalloc().
 */
void *vzalloc(size_t size) {
  return vmalloc(size);
}

/**
 * vfree - Free vmalloc memory
 */
void vfree(void *addr) {
  if (!addr) return;

  uint64_t vaddr = (uint64_t) addr;
  if (vaddr < VMALLOC_VIRT_BASE || vaddr >= VMALLOC_VIRT_END) {
    printk(KERN_WARNING "vfree: address %p is outside vmalloc range\n", addr);
    return;
  }

  down_write(&init_mm.mmap_lock);

  struct vm_area_struct *vma = vma_find(&init_mm, vaddr);
  if (!vma || vma->vm_start != vaddr) {
    up_write(&init_mm.mmap_lock);
    printk(KERN_ERR "vfree: bad address %p or double free\n", addr);
    return;
  }

  vma_remove(&init_mm, vma);
  vmalloc_unmap_pages(vma);

  up_write(&init_mm.mmap_lock);
  
  // This will put the vm_object, which will free all fault-allocated pages.
  vma_free(vma);
}

/**
 * viomap - Map a physical address range into vmalloc space (uncached/device)
 */
void *viomap(uint64_t phys_addr, size_t size) {
  if (size == 0) return NULL;

  uint64_t offset = phys_addr & ~PAGE_MASK;
  uint64_t phys_start = phys_addr & PAGE_MASK;
  size_t page_aligned_size = PAGE_ALIGN_UP(size + offset);
  size_t reserve_size = page_aligned_size + PAGE_SIZE; // Guard page

  down_write(&init_mm.mmap_lock);

  uint64_t virt_start = vma_find_free_region(&init_mm, reserve_size,
                                             VMALLOC_VIRT_BASE,
                                             VMALLOC_VIRT_END);

  if (!virt_start) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  struct vm_area_struct *vma = vma_create(virt_start, virt_start + reserve_size,
                                          VM_READ | VM_WRITE | VM_IO);
  if (!vma) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  vma->vm_obj = vm_object_device_create(phys_start, page_aligned_size);
  if (!vma->vm_obj) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  if (vma_insert(&init_mm, vma) < 0) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  // IO mappings are always eager
  vmm_map_pages(g_kernel_pml4, virt_start, phys_start,
                page_aligned_size / PAGE_SIZE,
                PTE_PRESENT | PTE_RW | VMM_CACHE_UC | PTE_NX);

  up_write(&init_mm.mmap_lock);
  __asm__ volatile("mfence" ::: "memory");

  return (void *) (virt_start + offset);
}

/**
 * viomap_wc - Map a physical address range using Write-Combining
 */
void *viomap_wc(uint64_t phys_addr, size_t size) {
  if (size == 0) return NULL;

  uint64_t offset = phys_addr & ~PAGE_MASK;
  uint64_t phys_start = phys_addr & PAGE_MASK;
  size_t page_aligned_size = PAGE_ALIGN_UP(size + offset);
  size_t reserve_size = page_aligned_size + PAGE_SIZE;

  down_write(&init_mm.mmap_lock);

  uint64_t virt_start = vma_find_free_region(&init_mm, reserve_size,
                                             VMALLOC_VIRT_BASE,
                                             VMALLOC_VIRT_END);

  if (!virt_start) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  struct vm_area_struct *vma = vma_create(virt_start, virt_start + reserve_size,
                                          VM_READ | VM_WRITE | VM_IO | VM_CACHE_WC);
  if (!vma) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  vma->vm_obj = vm_object_device_create(phys_start, page_aligned_size);
  if (!vma->vm_obj) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  if (vma_insert(&init_mm, vma) < 0) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  vmm_map_pages(g_kernel_pml4, virt_start, phys_start,
                page_aligned_size / PAGE_SIZE,
                PTE_PRESENT | PTE_RW | VMM_CACHE_WC | PTE_NX);

  up_write(&init_mm.mmap_lock);
  __asm__ volatile("mfence" ::: "memory");

  return (void *) (virt_start + offset);
}

void viounmap(void *addr) {
  uint64_t vaddr_aligned = (uint64_t) addr & PAGE_MASK;
  vfree((void *) vaddr_aligned);
}

EXPORT_SYMBOL(vmalloc);
EXPORT_SYMBOL(vzalloc);
EXPORT_SYMBOL(vfree);
EXPORT_SYMBOL(vmalloc_exec);
EXPORT_SYMBOL(viomap_wc);
EXPORT_SYMBOL(viomap);
EXPORT_SYMBOL(viounmap);

/* ========================================================================
 * Vmalloc Stress Test
 * ========================================================================
 */

static uint32_t vm_seed = 0xDEADBEEF;

static uint32_t vm_rand(void) {
  vm_seed = vm_seed * 1103515245 + 12345;
  return (uint32_t) (vm_seed / 65536) % 32768;
}

#define STRESS_ALLOCS 128
#define STRESS_ITERS  10

void vmalloc_test(void) {
  printk(KERN_DEBUG VMM_CLASS "Starting vmalloc stress test...\n");

  void **ptrs = kmalloc(STRESS_ALLOCS * sizeof(void *));
  size_t *sizes = kmalloc(STRESS_ALLOCS * sizeof(size_t));
  if (!ptrs || !sizes) {
    printk(KERN_DEBUG VMM_CLASS "Failed to allocate test tracking arrays\n");
    return;
  }

  memset(ptrs, 0, STRESS_ALLOCS * sizeof(void *));
  memset(sizes, 0, STRESS_ALLOCS * sizeof(size_t));

  for (int iter = 0; iter < STRESS_ITERS; iter++) {
    // 1. Fill empty slots
    for (int i = 0; i < STRESS_ALLOCS; i++) {
      if (ptrs[i]) continue;

      uint32_t r = vm_rand() % 100;
      size_t size;
      if (r < 60) {
        size = PAGE_SIZE * ((vm_rand() % 16) + 1);
      } else if (r < 90) {
        size = PAGE_SIZE * ((vm_rand() % 240) + 16);
      } else {
        // Massive allocation test: 10MB - 100MB
        size = VMM_PAGE_SIZE_2M * ((vm_rand() % 45) + 5);
      }

      ptrs[i] = vzalloc(size); // Test zeroing
      sizes[i] = size;

      if (ptrs[i]) {
        uint8_t *buf = (uint8_t *) ptrs[i];
        // Verify it was zeroed (due to vzalloc calling vmalloc with lazy faulting)
        if (buf[0] != 0 || buf[size - 1] != 0) {
           panic("vzalloc failed to provide zeroed memory");
        }
        
        // Write pattern
        buf[0] = 0xAA;
        buf[size - 1] = 0xBB;
      }
    }

    // 2. Verify and Free Randomly
    for (int i = 0; i < STRESS_ALLOCS; i++) {
      if (!ptrs[i]) continue;

      uint8_t *buf = (uint8_t *) ptrs[i];
      if (buf[0] != 0xAA || buf[sizes[i] - 1] != 0xBB) {
        panic(VMM_CLASS "Memory corruption detected");
      }

      if (vm_rand() % 2) {
        vfree(ptrs[i]);
        ptrs[i] = NULL;
        sizes[i] = 0;
      }
    }
  }

  // Cleanup rest
  for (int i = 0; i < STRESS_ALLOCS; i++) {
    if (ptrs[i]) {
      vfree(ptrs[i]);
    }
  }

  kfree(ptrs);
  kfree(sizes);
  printk(KERN_DEBUG VMM_CLASS "vmalloc passed successfully.\n");
}