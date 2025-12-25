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

#include <printk.h>
#include <mm/vmalloc.h>
#include <mm/mm_types.h>
#include <mm/vma.h>
#include <arch/x64/mm/layout.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/spinlock.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/x64/mm/paging.h>
#include <kernel/panic.h>

/*
 * Internal helper to map physical pages to a VMA.
 * Uses bulk mapping if possible, otherwise falls back to slow path.
 */
static int vmalloc_map_pages(struct vm_area_struct *vma, uint64_t vmm_flags) {
    size_t count = vma_pages(vma);
    uint64_t virt_start = vma->vm_start;
    
    // Try to use bulk mapping if the list fits in a slab (up to 256 pages / 1MB)
    uint64_t *phys_list = kmalloc(count * sizeof(uint64_t));
    
    if (phys_list) {
        for (size_t i = 0; i < count; i++) {
            phys_list[i] = pmm_alloc_page();
            if (!phys_list[i]) {
                for (size_t j = 0; j < i; j++) pmm_free_page(phys_list[j]);
                kfree(phys_list);
                return -1;
            }
        }

        if (vmm_map_pages_list(g_kernel_pml4, virt_start, phys_list, count, vmm_flags | PTE_PRESENT) < 0) {
            for (size_t i = 0; i < count; i++) pmm_free_page(phys_list[i]);
            kfree(phys_list);
            return -1;
        }
        kfree(phys_list);
    } else {
        // Slow Path: Page-by-page mapping (for very large allocations > 1MB)
        for (size_t i = 0; i < count; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys || vmm_map_page(g_kernel_pml4, virt_start + i * PAGE_SIZE, phys, vmm_flags | PTE_PRESENT) < 0) {
                if (phys) pmm_free_page(phys);
                // Rollback previously mapped pages in this VMA
                for (size_t j = 0; j < i; j++) {
                    uint64_t v = virt_start + j * PAGE_SIZE;
                    uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
                    vmm_unmap_page(g_kernel_pml4, v);
                    if (p) pmm_free_page(p);
                }
                return -1;
            }
        }
    }

    return 0;
}

/*
 * Internal helper to unmap and free physical pages.
 */
static void vmalloc_unmap_pages(struct vm_area_struct *vma) {
    uint64_t virt = vma->vm_start;
    size_t count = vma_pages(vma);

    if (vma->vm_flags & VM_IO) {
        vmm_unmap_pages(g_kernel_pml4, virt, count);
        return;
    }

    uint64_t *phys_list = kmalloc(count * sizeof(uint64_t));
    if (phys_list) {
        vmm_unmap_pages_and_get_phys(g_kernel_pml4, virt, phys_list, count);
        for (size_t i = 0; i < count; i++) {
            if (phys_list[i]) pmm_free_page(phys_list[i]);
        }
        kfree(phys_list);
    } else {
        // Slow path: unmap and free one by one
        for (size_t i = 0; i < count; i++) {
            uint64_t v = virt + i * PAGE_SIZE;
            uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
            vmm_unmap_page(g_kernel_pml4, v);
            if (p) pmm_free_page(p);
        }
    }
}

static void *__vmalloc_internal(size_t size, uint64_t vma_flags, uint64_t vmm_flags) {
    if (size == 0) return NULL;

    size = PAGE_ALIGN_UP(size);

    spinlock_lock(&init_mm.mmap_lock);

    uint64_t virt_start = vma_find_free_region(&init_mm, size, 
                                               VMALLOC_VIRT_BASE, 
                                               VMALLOC_VIRT_END);

    if (virt_start == 0) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    struct vm_area_struct *vma = vma_create(virt_start, virt_start + size, vma_flags);
    if (!vma) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    if (vma_insert(&init_mm, vma) < 0) {
        vma_free(vma);
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    if (vmalloc_map_pages(vma, vmm_flags) < 0) {
        vma_remove(&init_mm, vma);
        vma_free(vma);
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

    spinlock_unlock(&init_mm.mmap_lock);
    return (void *)virt_start;
}

void *vmalloc(size_t size) {
    return __vmalloc_internal(size, VM_READ | VM_WRITE, PTE_RW);
}

void *vmalloc_exec(size_t size) {
    return __vmalloc_internal(size, VM_READ | VM_WRITE | VM_EXEC, PTE_RW);
}

void *vzalloc(size_t size) {
    void *ptr = vmalloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void vfree(void *addr) {
    if (!addr) return;

    uint64_t vaddr = (uint64_t)addr;
    if (vaddr < VMALLOC_VIRT_BASE || vaddr >= VMALLOC_VIRT_END) return;

    spinlock_lock(&init_mm.mmap_lock);

    struct vm_area_struct *vma = vma_find(&init_mm, vaddr);
    if (!vma || vma->vm_start != vaddr) {
        spinlock_unlock(&init_mm.mmap_lock);
        return;
    }

    vma_remove(&init_mm, vma);
    vmalloc_unmap_pages(vma);

    spinlock_unlock(&init_mm.mmap_lock);
    vma_free(vma);
}

void *viomap(uint64_t phys_addr, size_t size) {
    if (size == 0) return NULL;
    
    uint64_t offset = phys_addr & ~PAGE_MASK;
    uint64_t phys_start = phys_addr & PAGE_MASK;
    size_t page_aligned_size = PAGE_ALIGN_UP(size + offset);

    spinlock_lock(&init_mm.mmap_lock);

    uint64_t virt_start = vma_find_free_region(&init_mm, page_aligned_size, 
                                               VMALLOC_VIRT_BASE, 
                                               VMALLOC_VIRT_END);
    
    if (!virt_start) {
        spinlock_unlock(&init_mm.mmap_lock);
        return NULL;
    }

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

    vmm_map_pages(g_kernel_pml4, virt_start, phys_start, 
                 page_aligned_size / PAGE_SIZE,
                 PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT);

    spinlock_unlock(&init_mm.mmap_lock);
    __asm__ volatile("mfence" ::: "memory");

    return (void *)(virt_start + offset);
}

void viounmap(void *addr) {
    uint64_t vaddr_aligned = (uint64_t)addr & PAGE_MASK;
    vfree((void *)vaddr_aligned);
}