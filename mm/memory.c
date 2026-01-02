/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/memory.c
 * @brief High-level memory management and fault handling
 * @copyright (C) 2025 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/vma.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <lib/string.h>
#include <lib/printk.h>

/* Fault flags */
#define FAULT_FLAG_WRITE 0x01
#define FAULT_FLAG_USER  0x02
#define FAULT_FLAG_INSTR 0x04

/* Standard fault return codes */
#define VM_FAULT_OOM    0x0001
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_SIGSEGV 0x0004
#define VM_FAULT_MAJOR  0x0008
#define VM_FAULT_RETRY  0x0010

/* Global LRU Lists */
static struct list_head inactive_list;
static struct list_head active_list;
static spinlock_t lru_lock = 0;

void lru_init(void) {
    INIT_LIST_HEAD(&inactive_list);
    INIT_LIST_HEAD(&active_list);
    spinlock_init(&lru_lock);
}

void lru_add(struct page *page) {
    irq_flags_t flags = spinlock_lock_irqsave(&lru_lock);
    list_add(&page->lru, &inactive_list);
    page->flags |= PG_lru;
    spinlock_unlock_irqrestore(&lru_lock, flags);
}

/**
 * anon_vma_chain_link
 * Connects a VMA to an anon_vma via a chain node.
 */
int anon_vma_chain_link(struct vm_area_struct *vma, struct anon_vma *av) {
    struct anon_vma_chain *avc = kmalloc(sizeof(struct anon_vma_chain));
    if (!avc) return -ENOMEM;

    avc->vma = vma;
    avc->anon_vma = av;
    
    /* Link into the VMA's list of chains */
    list_add(&avc->same_vma, &vma->anon_vma_chain);
    
    /* Link into the anon_vma's list of VMAs */
    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
    list_add(&avc->same_anon_vma, &av->head);
    atomic_inc(&av->refcount);
    spinlock_unlock_irqrestore(&av->lock, flags);

    return 0;
}

/**
 * Ensure a VMA has an anon_vma for reverse mapping.
 */
int anon_vma_prepare(struct vm_area_struct *vma) {
    if (vma->anon_vma) return 0;

    struct anon_vma *av = kmalloc(sizeof(struct anon_vma));
    if (!av) return -ENOMEM;

    spinlock_init(&av->lock);
    INIT_LIST_HEAD(&av->head);
    atomic_set(&av->refcount, 1);
    av->parent = NULL;

    vma->anon_vma = av;
    return anon_vma_chain_link(vma, av);
}

/**
 * Links a physical page to an anonymous VMA for reverse mapping.
 */
void page_add_anon_rmap(struct page *page, struct vm_area_struct *vma, uint64_t address) {
    if (!page->mapping) {
        /* Set bit 0 to indicate anonymous mapping */
        page->mapping = (void *)((uintptr_t)vma->anon_vma | 0x1);
        page->index = (address - vma->vm_start) >> PAGE_SHIFT;
        
        /* Add to global LRU list for reclamation */
        lru_add(page);
    }
}

/**
 * Handle a page fault for anonymous memory.
 */
static int anon_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
    if (anon_vma_prepare(vma) < 0) return VM_FAULT_OOM;

    uint64_t phys = pmm_alloc_page();
    if (!phys) return VM_FAULT_OOM;

    struct page *page = phys_to_page(phys);
    memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);

    page_add_anon_rmap(page, vma, vmf->address);
    
    vmf->page = page;
    return 0;
}

void anon_vma_free(struct anon_vma *av) {
    if (!av) return;
    if (atomic_dec_and_test(&av->refcount)) {
        kfree(av);
    }
}

/**
 * try_to_unmap - The core of memory reclamation.
 * Unmaps a page from all VMAs that reference it.
 */
int try_to_unmap(struct page *page) {
    if (!page->mapping) return 0;

    /* Check if it's anonymous (bit 0 set) */
    if ((uintptr_t)page->mapping & 0x1) {
        struct anon_vma *av = (struct anon_vma *)((uintptr_t)page->mapping & ~0x1);
        
        irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
        struct anon_vma_chain *avc;
        
        list_for_each_entry(avc, &av->head, same_anon_vma) {
            struct vm_area_struct *vma = avc->vma;
            uint64_t address = vma->vm_start + (page->index << PAGE_SHIFT);
            
            if (address < vma->vm_start || address >= vma->vm_end) continue;

            /* Unmap from this process's page table */
            if (vma->vm_mm->pml4) {
                vmm_unmap_page((uint64_t)vma->vm_mm->pml4, address);
            }
        }
        
        spinlock_unlock_irqrestore(&av->lock, flags);
        page->mapping = NULL;
        return 1;
    }

    return 0;
}

const struct vm_operations_struct anon_vm_ops = {
    .fault = anon_fault,
};

/* --- Shared Memory Support (Proof of Concept) --- */

struct shmem_obj {
    struct page **pages;
    size_t nr_pages;
};

static int shmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
    struct shmem_obj *obj = vma->vm_private_data;
    if (!obj || vmf->pgoff >= obj->nr_pages) return VM_FAULT_SIGBUS;

    struct page *page = obj->pages[vmf->pgoff];
    if (!page) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return VM_FAULT_OOM;
        page = phys_to_page(phys);
        memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);
        obj->pages[vmf->pgoff] = page;
    }

    /* Important: Increment refcount because another mapping now points to it */
    get_page(page);
    
    vmf->page = page;
    return 0;
}

const struct vm_operations_struct shmem_vm_ops = {
    .fault = shmem_fault,
};

/**
 * Generic fault handler that dispatches to VMA-specific operations.
 */
int handle_mm_fault(struct vm_area_struct *vma, uint64_t address, unsigned int flags) {
    struct vm_fault vmf;
    vmf.address = address & PAGE_MASK;
    vmf.flags = flags;
    vmf.pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
    vmf.page = NULL;

    const struct vm_operations_struct *ops = vma->vm_ops;
    
    /* Default to anonymous fault if no ops provided */
    if (!ops || !ops->fault) {
        ops = &anon_vm_ops;
    }

    int ret = ops->fault(vma, &vmf);
    if (ret != 0) return ret;

    /* Map the page into the process's page table */
    if (vmf.page) {
        uint64_t pte_flags = PTE_PRESENT;
        if (vma->vm_flags & VM_USER) pte_flags |= PTE_USER;
        if (vma->vm_flags & VM_WRITE) pte_flags |= PTE_RW;
        if (!(vma->vm_flags & VM_EXEC)) pte_flags |= PTE_NX;

        uint64_t phys = PFN_TO_PHYS(page_to_pfn(vmf.page));
        vmm_map_page((uint64_t)vma->vm_mm->pml4, vmf.address, phys, pte_flags);
    }

    return 0;
}
