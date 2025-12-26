/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vmalloc.c
 * @brief Kernel virtual memory allocation implementation (Huge Page Aware)
 * @copyright (C) 2025 assembler-0
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
 * Now supports Huge Page (2MB) mappings.
 */
static int vmalloc_map_pages(struct vm_area_struct *vma, uint64_t vmm_flags) {
    size_t count = vma_pages(vma);
    uint64_t virt_start = vma->vm_start;
    
    if (vma->vm_flags & VM_HUGE) {
        // Huge Page Path (2MB)
        size_t huge_count = count / 512;
        for (size_t i = 0; i < huge_count; i++) {
            uint64_t phys = pmm_alloc_pages(512); // Allocate 2MB contiguous
            if (!phys || vmm_map_huge_page(g_kernel_pml4, virt_start + i * VMM_PAGE_SIZE_2M, 
                                           phys, vmm_flags | PTE_PRESENT, VMM_PAGE_SIZE_2M) < 0) {
                if (phys) pmm_free_pages(phys, 512);
                // Rollback
                for (size_t j = 0; j < i; j++) {
                    uint64_t v = virt_start + j * VMM_PAGE_SIZE_2M;
                    uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
                    vmm_unmap_page(g_kernel_pml4, v);
                    if (p) pmm_free_pages(p, 512);
                }
                return -1;
            }
        }
        return 0;
    }

    // Standard 4KB Path
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
        // Slow Path: Page-by-page mapping
        for (size_t i = 0; i < count; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys || vmm_map_page(g_kernel_pml4, virt_start + i * PAGE_SIZE, phys, vmm_flags | PTE_PRESENT) < 0) {
                if (phys) pmm_free_page(phys);
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

    if (vma->vm_flags & VM_HUGE) {
        size_t huge_count = count / 512;
        for (size_t i = 0; i < huge_count; i++) {
            uint64_t v = virt + i * VMM_PAGE_SIZE_2M;
            uint64_t p = vmm_virt_to_phys(g_kernel_pml4, v);
            vmm_unmap_page(g_kernel_pml4, v);
            if (p) pmm_free_pages(p, 512);
        }
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
    
    // Opportunistically use 2MB Huge Pages for allocations >= 2MB
    uint64_t alignment = PAGE_SIZE;
    if (size >= VMM_PAGE_SIZE_2M) {
        alignment = VMM_PAGE_SIZE_2M;
        size = (size + VMM_PAGE_SIZE_2M - 1) & ~(VMM_PAGE_SIZE_2M - 1);
        vma_flags |= VM_HUGE;
    }

    spinlock_lock(&init_mm.mmap_lock);

    uint64_t virt_start = vma_find_free_region_aligned(&init_mm, size, alignment, 
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
                 PTE_PRESENT | PTE_RW | VMM_CACHE_UC);

    spinlock_unlock(&init_mm.mmap_lock);
    __asm__ volatile("mfence" ::: "memory");

    return (void *)(virt_start + offset);
}

void *viomap_wc(uint64_t phys_addr, size_t size) {
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
                                            VM_READ | VM_WRITE | VM_IO | VM_CACHE_WC);
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
                 PTE_PRESENT | PTE_RW | VMM_CACHE_WC);

    spinlock_unlock(&init_mm.mmap_lock);
    __asm__ volatile("mfence" ::: "memory");

    return (void *)(virt_start + offset);
}

void viounmap(void *addr) {
    uint64_t vaddr_aligned = (uint64_t)addr & PAGE_MASK;
    vfree((void *)vaddr_aligned);
}
