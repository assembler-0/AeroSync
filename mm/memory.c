///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/memory.c
 * @brief High-level memory management, Folios, and RMAP
 * @copyright (C) 2025 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/vma.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <lib/string.h>
#include <lib/printk.h>

#include <kernel/wait.h>
#include <kernel/sched/process.h>

/* Global LRU Lists */
static struct list_head inactive_list;
static struct list_head active_list;
static spinlock_t lru_lock = 0;

static DECLARE_WAIT_QUEUE_HEAD(kswapd_wait);
static struct zone *volatile kswapd_zone = NULL;

/**
 * folio_add_lru - Add a folio to the inactive LRU list.
 */
void folio_add_lru(struct folio *folio) {
    irq_flags_t flags = spinlock_lock_irqsave(&lru_lock);
    if (!(folio->page.flags & PG_lru)) {
        list_add(&folio->page.lru, &inactive_list);
        folio->page.flags |= PG_lru;
    }
    spinlock_unlock_irqrestore(&lru_lock, flags);
}

/**
 * try_to_unmap_folio - The core of memory reclamation.
 * Unmaps a folio from all VMAs that reference it.
 */
int try_to_unmap_folio(struct folio *folio) {
    if (!folio->page.mapping) return 0;

    /* Check if it's anonymous (bit 0 set) */
    if ((uintptr_t)folio->page.mapping & 0x1) {
        struct anon_vma *av = (struct anon_vma *)((uintptr_t)folio->page.mapping & ~0x1);
        
        irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
        struct anon_vma_chain *avc;
        
        list_for_each_entry(avc, &av->head, same_anon_vma) {
            struct vm_area_struct *vma = avc->vma;
            uint64_t address = vma->vm_start + (folio->page.index << PAGE_SHIFT);
            
            if (address < vma->vm_start || address >= vma->vm_end) continue;

            /* Unmap from this process's page table */
            if (vma->vm_mm->pml4) {
                vmm_unmap_page((uint64_t)vma->vm_mm->pml4, address);
            }
        }
        
        spinlock_unlock_irqrestore(&av->lock, flags);
        folio->page.mapping = NULL;
        return 1;
    }

    return 0;
}

/**
 * folio_referenced - Check if any PTE pointing to this folio has the Accessed bit set.
 */
int folio_referenced(struct folio *folio) {
    int referenced = 0;
    if (!folio->page.mapping) return 0;

    if ((uintptr_t)folio->page.mapping & 0x1) {
        struct anon_vma *av = (struct anon_vma *)((uintptr_t)folio->page.mapping & ~0x1);
        irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
        struct anon_vma_chain *avc;

        list_for_each_entry(avc, &av->head, same_anon_vma) {
            struct vm_area_struct *vma = avc->vma;
            uint64_t address = vma->vm_start + (folio->page.index << PAGE_SHIFT);
            
            if (address < vma->vm_start || address >= vma->vm_end) continue;

            if (vma->vm_mm->pml4) {
                if (vmm_is_accessed((uint64_t)vma->vm_mm->pml4, address)) {
                    referenced++;
                    vmm_clear_accessed((uint64_t)vma->vm_mm->pml4, address);
                }
            }
        }
        spinlock_unlock_irqrestore(&av->lock, flags);
    }

    return referenced;
}

/**
 * folio_reclaim - Attempt to free a folio by unmapping it from all users.
 */
int folio_reclaim(struct folio *folio) {
    if (folio_referenced(folio)) {
        irq_flags_t flags = spinlock_lock_irqsave(&lru_lock);
        list_move(&folio->page.lru, &active_list);
        folio->page.flags |= PG_active;
        spinlock_unlock_irqrestore(&lru_lock, flags);
        return -1;
    }

    if (try_to_unmap_folio(folio)) {
        uint64_t phys = folio_to_phys(folio);
        pmm_free_page(phys);
        return 0;
    }

    return -1;
}

/**
 * shrink_inactive_list - Scan the inactive LRU for pages to reclaim.
 */
static size_t shrink_inactive_list(size_t nr_to_scan) {
    size_t reclaimed = 0;
    struct list_head page_list;
    INIT_LIST_HEAD(&page_list);

    irq_flags_t flags = spinlock_lock_irqsave(&lru_lock);
    for (size_t i = 0; i < nr_to_scan && !list_empty(&inactive_list); i++) {
        struct page *page = list_last_entry(&inactive_list, struct page, lru);
        list_move(&page->lru, &page_list);
    }
    spinlock_unlock_irqrestore(&lru_lock, flags);

    struct page *page, *tmp;
    list_for_each_entry_safe(page, tmp, &page_list, lru) {
        struct folio *folio = page_folio(page);
        if (folio_reclaim(folio) == 0) {
            reclaimed++;
        }
    }

    return reclaimed;
}

void wakeup_kswapd(struct zone *zone) {
    if (!zone) return;
    if (kswapd_zone == NULL) {
        kswapd_zone = zone;
        wake_up(&kswapd_wait);
    }
}

static int kswapd_should_run(void) {
    return kswapd_zone != NULL;
}

static int kswapd_thread(void *data) {
    (void)data;
    printk(KERN_INFO SWAP_CLASS "kswapd started\n");

    while (1) {
        wait_event(&kswapd_wait, kswapd_should_run());

        struct zone *z = kswapd_zone;
        if (!z) continue;

        /* Reclaim until we hit high watermark */
        int loops = 0;
        while (z->nr_free_pages < z->watermark[WMARK_HIGH]) {
            size_t reclaimed = shrink_inactive_list(32);
            if (reclaimed == 0) break;
            if (++loops > 1000) break;
        }

        kswapd_zone = NULL;
    }
    return 0;
}

void kswapd_init(void) {
    struct task_struct *k = kthread_create(kswapd_thread, NULL, "kswapd");
    if (k) kthread_run(k);
}

void lru_init(void) {
    INIT_LIST_HEAD(&inactive_list);
    INIT_LIST_HEAD(&active_list);
    spinlock_init(&lru_lock);
}

/**
 * anon_vma_chain_link - Connects a VMA to an anon_vma via a chain node.
 */
int anon_vma_chain_link(struct vm_area_struct *vma, struct anon_vma *av) {
    struct anon_vma_chain *avc = kmalloc(sizeof(struct anon_vma_chain));
    if (!avc) return -ENOMEM;

    avc->vma = vma;
    avc->anon_vma = av;
    list_add(&avc->same_vma, &vma->anon_vma_chain);
    
    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
    list_add(&avc->same_anon_vma, &av->head);
    atomic_inc(&av->refcount);
    spinlock_unlock_irqrestore(&av->lock, flags);

    return 0;
}

/**
 * anon_vma_prepare - Ensure a VMA has an anon_vma for reverse mapping.
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

void anon_vma_free(struct anon_vma *av) {
    if (!av) return;
    if (atomic_dec_and_test(&av->refcount)) {
        kfree(av);
    }
}

/**
 * folio_add_anon_rmap - Links a physical folio to an anonymous VMA.
 */
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address) {
    if (!folio->page.mapping) {
        folio->page.mapping = (void *)((uintptr_t)vma->anon_vma | 0x1);
        folio->page.index = (address - vma->vm_start) >> PAGE_SHIFT;
        folio_add_lru(folio);
    }
}

/**
 * Handle a page fault for anonymous memory.
 */
static int anon_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
    if (anon_vma_prepare(vma) < 0) return VM_FAULT_OOM;

    uint64_t phys = pmm_alloc_page();
    if (!phys) return VM_FAULT_OOM;

    struct folio *folio = page_folio(phys_to_page(phys));
    memset(folio_address(folio), 0, PAGE_SIZE);

    folio_add_anon_rmap(folio, vma, vmf->address);
    vmf->page = &folio->page;
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
    if (!ops || !ops->fault) {
        ops = &anon_vm_ops;
    }

    int ret = ops->fault(vma, &vmf);
    if (ret != 0) return ret;

    if (vmf.page) {
        uint64_t pte_flags = PTE_PRESENT;
        if (vma->vm_flags & VM_USER) pte_flags |= PTE_USER;
        if (vma->vm_flags & VM_WRITE) pte_flags |= PTE_RW;
        if (!(vma->vm_flags & VM_EXEC)) pte_flags |= PTE_NX;

        struct folio *folio = page_folio(vmf.page);
        uint64_t phys = folio_to_phys(folio);
        vmm_map_page((uint64_t)vma->vm_mm->pml4, vmf.address, phys, pte_flags);
    }

    return 0;
}
