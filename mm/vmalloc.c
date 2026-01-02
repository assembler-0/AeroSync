///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vmalloc.c
 * @brief Kernel Virtual Memory Allocation
 * @copyright (C) 2025 assembler-0
 *
 * Features:
 * - Red-Black Tree VMA management (via init_mm)
 * - Guard Pages for buffer overflow detection
 * - Batched mapping (avoids large kmallocs for metadata)
 * - Huge Page (2MB) support for large allocations
 * - Lazy unmapping support (architecture prepared)
 * - NX (No-Execute) by default for security
 * - Robust error handling and rollback
 */

#include <lib/printk.h>
#include <mm/vmalloc.h>
#include <mm/mm_types.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <arch/x64/mm/layout.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/x64/mm/paging.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/fkx/fkx.h>

#define VMALLOC_BATCH_SIZE 64

/*
 * Internal helper to map physical pages to a VMA.
 * Uses a small stack buffer to avoid massive kmalloc() for page arrays.
 * Respects the 'real_size' which might be smaller than vma->vm_end - vma->vm_start
 * due to guard pages.
 */
static int vmalloc_map_pages_batched(struct vm_area_struct *vma, size_t real_size, uint64_t vmm_flags) {
  uint64_t virt_start = vma->vm_start;
  size_t total_pages = real_size >> PAGE_SHIFT;
  size_t pages_mapped = 0;

  // Huge Page Path (2MB) - Opportunistic
  if (vma->vm_flags & VM_HUGE) {
    size_t huge_count = total_pages / 512;
    for (size_t i = 0; i < huge_count; i++) {
      struct folio *folio = alloc_pages(GFP_KERNEL, 9); // order 9 = 512 pages = 2MB
      if (!folio) goto rollback_huge;
      
      uint64_t phys = folio_to_phys(folio);
      if (vmm_map_huge_page(g_kernel_pml4, virt_start + i * VMM_PAGE_SIZE_2M,
                                     phys, vmm_flags | PTE_PRESENT, VMM_PAGE_SIZE_2M) < 0) {
        folio_put(folio);
        goto rollback_huge;
      }
      pages_mapped += 512;
    }
    // Handle remaining 4K pages if any (unlikely with strict alignment, but safe to have)
    virt_start += huge_count * VMM_PAGE_SIZE_2M;
    total_pages -= huge_count * 512;
  }

  // Standard 4KB Path (Batched)
  uint64_t phys_batch[VMALLOC_BATCH_SIZE];

  while (total_pages > 0) {
    size_t batch_count = (total_pages > VMALLOC_BATCH_SIZE) ? VMALLOC_BATCH_SIZE : total_pages;

    // 1. Allocate a batch of pages
    for (size_t i = 0; i < batch_count; i++) {
      phys_batch[i] = pmm_alloc_page();
      if (!phys_batch[i]) {
        // Free current partial batch
        for (size_t j = 0; j < i; j++) pmm_free_page(phys_batch[j]);
        goto rollback_standard;
      }
    }

    // 2. Map the batch
    if (vmm_map_pages_list(g_kernel_pml4, virt_start, phys_batch, batch_count, vmm_flags | PTE_PRESENT) < 0) {
      for (size_t i = 0; i < batch_count; i++) pmm_free_page(phys_batch[i]);
      goto rollback_standard;
    }

    virt_start += batch_count * PAGE_SIZE;
    total_pages -= batch_count;
    pages_mapped += batch_count;
  }

  return 0;

rollback_standard:
  // Unmap everything mapped so far in 4K path
  // We rely on unmap to free the pages
  // But we must know HOW MANY were mapped.
  // The clean way is to let the caller handle full VMA cleanup via vfree logic,
  // but the VMA might not be fully constructed yet.
  // So we do a manual rollback here.
  {
    uint64_t cleanup_addr = vma->vm_start;
    // Skip huge pages for now, they are handled separately or before
    if (vma->vm_flags & VM_HUGE) {
      size_t huge_done = (pages_mapped / 512) * 512;
      cleanup_addr += huge_done * PAGE_SIZE;
      pages_mapped -= huge_done;
    }

    for (size_t i = 0; i < pages_mapped; i++) {
      uint64_t v = cleanup_addr + i * PAGE_SIZE;
      uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
      vmm_unmap_page(g_kernel_pml4, v);
      if (p) pmm_free_page(p);
    }
  }
  // Fallthrough to huge rollback

rollback_huge:
  if (vma->vm_flags & VM_HUGE) {
    size_t huge_count = (real_size >> PAGE_SHIFT) / 512;
    // We only mapped 'i' huge pages before failure.
    // We need to pass the failure index or track it.
    // For simplicity, we scan the whole range and unmap valid huge pages.
    for (size_t i = 0; i < huge_count; i++) {
      uint64_t v = vma->vm_start + i * VMM_PAGE_SIZE_2M;
      uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
      // Check if it was actually mapped (present)
      if (p) {
        vmm_unmap_page(g_kernel_pml4, v);
        pmm_free_pages(p, 512);
      }
    }
  }
  return -1;
}

/*
 * Unmaps and frees pages in a VMA.
 * Handles gaps (guard pages) gracefully by skipping non-present pages.
 */
static void vmalloc_unmap_pages(struct vm_area_struct *vma) {
  uint64_t virt = vma->vm_start;
  size_t count = vma_pages(vma); // Includes guard page

  // Safety: vmalloc/viomap always add 1 Guard Page at the end.
  // We must NOT try to unmap or free it.
  if (count > 0) count--;

  if (vma->vm_flags & VM_IO) {
    vmm_unmap_pages(g_kernel_pml4, virt, count);
    return;
  }

  if (vma->vm_flags & VM_HUGE) {
    size_t huge_count = count / 512;
    for (size_t i = 0; i < huge_count; i++) {
      uint64_t v = virt + i * VMM_PAGE_SIZE_2M;
      uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
      if (p) {
        vmm_unmap_page(g_kernel_pml4, v);
        pmm_free_pages(p, 512);
      }
    }
    return;
  }

  // Standard 4K path - Batched retrieval
  size_t remaining = count;
  uint64_t phys_batch[VMALLOC_BATCH_SIZE];

  while (remaining > 0) {
    size_t batch = (remaining > VMALLOC_BATCH_SIZE) ? VMALLOC_BATCH_SIZE : remaining;

    // Get PAs before unmapping
    vmm_unmap_pages_and_get_phys(g_kernel_pml4, virt, phys_batch, batch);

    for (size_t i = 0; i < batch; i++) {
      if (phys_batch[i]) {
        pmm_free_page(phys_batch[i]);
      }
    }

    virt += batch * PAGE_SIZE;
    remaining -= batch;
  }
}

static void *__vmalloc_internal(size_t size, uint64_t vma_flags, uint64_t vmm_flags) {
  if (size == 0) return NULL;

  // 1. Align the requested size
  size_t alloc_size = PAGE_ALIGN_UP(size);

  // 2. Determine Huge Page usage
  uint64_t alignment = PAGE_SIZE;
  if (alloc_size >= VMM_PAGE_SIZE_2M && !(vma_flags & VM_IO)) {
    // Only use huge pages if explicitly allowed or large enough and safe
    // For simplicity, we align to 2MB if size >= 2MB
    alignment = VMM_PAGE_SIZE_2M;
    // Align alloc_size up to 2MB to avoid fragmentation within the huge page
    alloc_size = (alloc_size + VMM_PAGE_SIZE_2M - 1) & ~(VMM_PAGE_SIZE_2M - 1);
    vma_flags |= VM_HUGE;
  }

  // 3. Add Guard Page (4KB) to the VIRTUAL reservation size
  // This creates a hole between this allocation and the next one.
  // NOTE: If using Huge Pages, the guard is effectively a 2MB hole or we break alignment.
  // For efficient packing, we stick to 4KB guard.
  // If VM_HUGE is set, we must be careful.
  // VMA Manager gap tracking handles alignment.
  // We reserve 'alloc_size + PAGE_SIZE'.
  size_t reserve_size = alloc_size + PAGE_SIZE;

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

  // 5. Create VMA
  struct vm_area_struct *vma = vma_create(virt_start, virt_start + reserve_size, vma_flags);
  if (!vma) {
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  if (vma_insert(&init_mm, vma) < 0) {
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  // 6. Map the physical pages (ONLY alloc_size, NOT reserve_size)
  // The last page (reserve_size - alloc_size) is the guard page and remains unmapped.
  if (vmalloc_map_pages_batched(vma, alloc_size, vmm_flags) < 0) {
    vma_remove(&init_mm, vma);
    vma_free(vma);
    up_write(&init_mm.mmap_lock);
    return NULL;
  }

  up_write(&init_mm.mmap_lock);

  // 7. KASAN Unpoison (if implemented)
  // kasan_unpoison_shadow((void*)virt_start, alloc_size);

  return (void *) virt_start;
}

/**
 * vmalloc - Allocate virtually contiguous memory
 * @size: Allocation size in bytes
 *
 * Allocates 'size' bytes of virtually contiguous memory.
 * The memory is not physically contiguous (unless Huge Pages are used).
 *
 * Security:
 * - Adds a guard page at the end of the allocation.
 * - Memory is NX (No-Execute) by default.
 */
void *vmalloc(size_t size) {
  // VM_READ | VM_WRITE implies NX in our standard PTE mapping unless VM_EXEC is present
  // We pass PTE_RW | PTE_NX to explicit mapping
  return __vmalloc_internal(size, VM_READ | VM_WRITE, PTE_RW | PTE_NX);
}

/**
 * vmalloc_exec - Allocate executable virtually contiguous memory
 * @size: Allocation size in bytes
 *
 * Use ONLY for kernel modules or JIT code.
 */
void *vmalloc_exec(size_t size) {
  return __vmalloc_internal(size, VM_READ | VM_WRITE | VM_EXEC, PTE_RW);
  // PTE_RW implies Executable usually unless NX set
}

/**
 * vzalloc - Allocate zeroed virtually contiguous memory
 * @size: Allocation size in bytes
 */
void *vzalloc(size_t size) {
  void *ptr = vmalloc(size);
  if (ptr) memset(ptr, 0, size); // TODO: Optimize by requesting zeroed pages from PMM
  return ptr;
}

/**
 * vfree - Free vmalloc memory
 * @addr: Pointer to the memory to free
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
  vma_free(vma);
}

/**
 * viomap - Map a physical address range into vmalloc space (uncached/device)
 * @phys_addr: Physical start address
 * @size: Size to map
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

  /* Create Device Object for UBC integration */
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

  // Map only the requested range, not the guard page
  vmm_map_pages(g_kernel_pml4, virt_start, phys_start,
                page_aligned_size / PAGE_SIZE,
                PTE_PRESENT | PTE_RW | VMM_CACHE_UC | PTE_NX);

  up_write(&init_mm.mmap_lock);
  __asm__ volatile("mfence" ::: "memory");

  return (void *) (virt_start + offset);
}

/**
 * viomap_wc - Map a physical address range using Write-Combining
 * @phys_addr: Physical start address
 * @size: Size to map
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
 * ======================================================================== */

static uint32_t vm_seed = 0xDEADBEEF;

static uint32_t vm_rand(void) {
  vm_seed = vm_seed * 1103515245 + 12345;
  return (uint32_t) (vm_seed / 65536) % 32768;
}

#define STRESS_ALLOCS 128
#define STRESS_ITERS  10

void vmalloc_test(void) {
  printk(KERN_DEBUG VMM_CLASS "Starting stress test...\n");

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

      // Random size strategy
      uint32_t r = vm_rand() % 100;
      size_t size;
      if (r < 60) {
        // Small: 4KB - 64KB
        size = PAGE_SIZE * ((vm_rand() % 16) + 1);
      } else if (r < 90) {
        // Medium: 64KB - 1MB
        size = PAGE_SIZE * ((vm_rand() % 240) + 16);
      } else {
        // Large/Huge: 2MB - 10MB
        size = VMM_PAGE_SIZE_2M * ((vm_rand() % 5) + 1);
        // Add some offset to test alignment handling
        if (vm_rand() % 2) size += (vm_rand() % 1024);
      }

      ptrs[i] = vmalloc(size);
      sizes[i] = size;

      if (ptrs[i]) {
        // Write pattern: each byte = (index ^ 0xAA)
        uint8_t *buf = (uint8_t *) ptrs[i];
        // Only verify/write first and last page to save time
        memset(buf, 0xAA, PAGE_SIZE); // First page
        if (size > PAGE_SIZE) {
          memset(buf + size - PAGE_SIZE, 0xBB, PAGE_SIZE); // Last page
        }
      }
    }

    // 2. Verify and Free Randomly
    for (int i = 0; i < STRESS_ALLOCS; i++) {
      if (!ptrs[i]) continue;

      uint8_t *buf = (uint8_t *) ptrs[i];
      // Verify
      if (buf[0] != 0xAA) {
        panic(VMM_CLASS "Memory corruption detected");
      }
      if (sizes[i] > PAGE_SIZE) {
        if (buf[sizes[i] - PAGE_SIZE] != 0xBB) {
          panic(VMM_CLASS "Memory corruption detected");
        }
      }

      // Free with 50% probability
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
  printk(KERN_DEBUG VMM_CLASS "Passed successfully.\n");
}
