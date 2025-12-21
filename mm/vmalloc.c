/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vmalloc.c
 * @brief Kernel virtual memory allocation implementation
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

#include <mm/vmalloc.h>
#include <mm/mm_types.h>
#include <mm/vma.h>
#include <arch/x64/mm/layout.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/spinlock.h>
#include <string.h>

/*
 * Internal helper to map physical pages to a VMA.
 * Returns 0 on success, -1 on failure.
 */
static int vmalloc_map_pages(struct vm_area_struct *vma, uint64_t vmm_flags) {
    uint64_t virt = vma->vm_start;
    uint64_t end = vma->vm_end;
    
    // We iterate page by page
    for (; virt < end; virt += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            goto rollback;
        }

        // Map the page in the kernel PML4
        if (vmm_map_page(g_kernel_pml4, virt, phys, vmm_flags) < 0) {
            pmm_free_page(phys);
            goto rollback;
        }
    }
    return 0;

rollback:
    for (uint64_t v = vma->vm_start; v < virt; v += PAGE_SIZE) {
        uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
        vmm_unmap_page(g_kernel_pml4, v);
        if (p) pmm_free_page(p);
    }
    return -1;
}

/*
 * Internal helper to unmap and free physical pages.
 */
static void vmalloc_unmap_pages(struct vm_area_struct *vma) {
    uint64_t virt = vma->vm_start;
    uint64_t end = vma->vm_end;

    for (; virt < end; virt += PAGE_SIZE) {
        // 1. Get Physical Address
        uint64_t phys = vmm_virt_to_phys(g_kernel_pml4, virt);
        
        // 2. Unmap virtual
        vmm_unmap_page(g_kernel_pml4, virt);
        
        // 3. Free physical (only if it was valid RAM, skip for MMIO)
        // Note: You might want a flag in VMA to distinguish IO mappings from RAM
        // For now, we assume if it's in VMALLOC space and not flagged IO, it's RAM.
        if (phys && !(vma->vm_flags & VM_IO)) {
            pmm_free_page(phys);
        }
    }
}

static void *__vmalloc_internal(size_t size, uint64_t vma_flags, uint64_t vmm_flags) {
    if (size == 0) return NULL;

    // 1. Align size to page boundary
    size = PAGE_ALIGN_UP(size);

    // 2. Lock the kernel memory descriptor
    spinlock_lock(&init_mm.mmap_lock);

    // 3. Find a hole in the VMALLOC region
    uint64_t virt_start = vma_find_free_region(&init_mm, size, 
                                               VMALLOC_VIRT_BASE, 
                                               VMALLOC_VIRT_END);

    if (virt_start == 0) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL; // No virtual address space left
    }

    // 4. Create the VMA
    struct vm_area_struct *vma = vma_create(virt_start, virt_start + size, vma_flags);
    if (!vma) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    // 5. Insert VMA into tree
    if (vma_insert(&init_mm, vma) < 0) {
        vma_free(vma);
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    spinlock_unlock(&init_mm.mmap_lock);

    // 6. Allocate and Map Physical Pages
    // We do this outside the lock if possible, though holding it prevents 
    // others from seeing partially mapped ranges. For kernel safety, 
    // we often hold it or use a specific loading state. 
    // Given your VMA design, it's safer to just map now.
    
    if (vmalloc_map_pages(vma, vmm_flags | PTE_PRESENT) < 0) {
        // Rollback!
        vfree((void*)virt_start); 
        return NULL;
    }

    return (void *)virt_start;
}

void *vmalloc(size_t size) {
    return __vmalloc_internal(size, 
        VM_READ | VM_WRITE, 
        PTE_RW);
}

void *vmalloc_exec(size_t size) {
    // Note: Assuming PTE_NX is supported and set by default on others.
    // If your vmm_map_page handles flags directly, pass nothing for NX (executable).
    return __vmalloc_internal(size, 
        VM_READ | VM_WRITE | VM_EXEC, 
        PTE_RW); // Omit PTE_NX if you have it defined, or ensure logic allows exec
}

void *vzalloc(size_t size) {
    void *ptr = vmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void vfree(void *addr) {
    if (!addr) return;

    uint64_t vaddr = (uint64_t)addr;

    // Sanity check: Ensure address is within VMALLOC range
    if (vaddr < VMALLOC_VIRT_BASE || vaddr >= VMALLOC_VIRT_END) {
        // Optional: print warning "vfree called on non-vmalloc address"
        return;
    }

    spinlock_lock(&init_mm.mmap_lock);

    // 1. Find the VMA
    struct vm_area_struct *vma = vma_find(&init_mm, vaddr);
    
    // Strict check: The address must match the start of the VMA
    if (!vma || vma->vm_start != vaddr) {
        spinlock_unlock(&init_mm.mmap_lock);
        return;
    }

    // 2. Remove from tree first to prevent access
    vma_remove(&init_mm, vma);

    spinlock_unlock(&init_mm.mmap_lock);

    // 3. Unmap and Free physical pages
    // (This is slow, so we do it after dropping the lock if strict consistency isn't required,
    // otherwise keep lock held)
    vmalloc_unmap_pages(vma);

    // 4. Free the VMA struct
    vma_free(vma);
}

/*
 * IO Mapping Implementation
 * Does not allocate PMM pages, just maps existing physical addresses.
 */
void *viomap(uint64_t phys_addr, size_t size) {
    if (size == 0) return NULL;
    
    uint64_t offset = phys_addr & ~PAGE_MASK;
    uint64_t phys_start = phys_addr & PAGE_MASK;
    size_t page_aligned_size = PAGE_ALIGN_UP(size + offset);

    spinlock_lock(&init_mm.mmap_lock);

    // 1. Find virtual range
    uint64_t virt_start = vma_find_free_region(&init_mm, page_aligned_size, 
                                               VMALLOC_VIRT_BASE, 
                                               VMALLOC_VIRT_END);
    
    if (!virt_start) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    // 2. Create VMA with IO flag
    struct vm_area_struct *vma = vma_create(virt_start, virt_start + page_aligned_size, 
                                            VM_READ | VM_WRITE | VM_IO);
    if (!vma) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    if (vma_insert(&init_mm, vma) < 0) {
        vma_free(vma);
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }
    spinlock_unlock(&init_mm.mmap_lock);

    // 3. Map the specific physical range
    // We map page by page manually or assume vmm_map_page can handle it.
    // Since vmm_map_page maps one page, we loop.
    for (uint64_t i = 0; i < page_aligned_size; i += PAGE_SIZE) {
        vmm_map_page(g_kernel_pml4, virt_start + i, phys_start + i, 
                     PTE_PRESENT | PTE_RW | PTE_PCD); // PCD for Cache Disable (IO)
    }

    return (void *)(virt_start + offset);
}

void viounmap(void *addr) {
    // viounmap is essentially vfree, but vfree handles the VM_IO flag
    // inside vmalloc_unmap_pages to ensure we don't try to free 
    // MMIO physical addresses to the PMM.
    
    // We need to align down to page boundary because viomap adds offset
    uint64_t vaddr_aligned = (uint64_t)addr & PAGE_MASK;
    vfree((void *)vaddr_aligned);
}