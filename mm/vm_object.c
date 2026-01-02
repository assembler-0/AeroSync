/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vm_object.c
 * @brief Virtual Memory Object management
 * @copyright (C) 2025 assembler-0
 */

#include <string.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/errno.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vm_object.h>

/* Forward declarations for RMAP (defined in memory.c) */
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address);

struct vm_object *vm_object_alloc(vm_object_type_t type) {
    struct vm_object *obj = kmalloc(sizeof(struct vm_object));
    if (!obj) return NULL;

    memset(obj, 0, sizeof(struct vm_object));
    obj->type = type;
    obj->page_tree = RB_ROOT;
    spinlock_init(&obj->lock);
    INIT_LIST_HEAD(&obj->i_mmap);
    atomic_set(&obj->refcount, 1);
    
    return obj;
}

void vm_object_free(struct vm_object *obj) {
    if (!obj) return;

    /* Free all pages in the tree */
    struct rb_node *node = rb_first(&obj->page_tree);
    while (node) {
        struct rb_node *next = rb_next(node);
        // We'll need a way to get the page from the rb_node
        // Assuming we store a wrapper or use the page's rb_node if we add it
        // For now, let's assume a simple wrapper for simplicity in this step
        // OR we can use page->mapping/index logic if we integrate it better.
        
        // TODO: Implement proper page tree cleanup
        node = next;
    }

    if (obj->ops && obj->ops->free) {
        obj->ops->free(obj);
    }

    kfree(obj);
}

void vm_object_get(struct vm_object *obj) {
    if (obj) atomic_inc(&obj->refcount);
}

void vm_object_put(struct vm_object *obj) {
    if (!obj) return;
    if (atomic_dec_and_test(&obj->refcount)) {
        vm_object_free(obj);
    }
}

/* Page Tree Management */

struct page_node {
    struct rb_node rb;
    uint64_t pgoff;
    struct page *page;
};

int vm_object_add_page(struct vm_object *obj, uint64_t pgoff, struct page *page) {
    struct rb_node **new = &obj->page_tree.rb_node, *parent = NULL;

    while (*new) {
        struct page_node *this = container_of(*new, struct page_node, rb);
        parent = *new;
        if (pgoff < this->pgoff)
            new = &((*new)->rb_left);
        else if (pgoff > this->pgoff)
            new = &((*new)->rb_right);
        else
            return -EEXIST;
    }

    struct page_node *node = kmalloc(sizeof(struct page_node));
    if (!node) return -ENOMEM;

    node->pgoff = pgoff;
    node->page = page;
    
    rb_link_node(&node->rb, parent, new);
    rb_insert_color(&node->rb, &obj->page_tree);

    /* Update page metadata */
    page->mapping = obj;
    page->index = pgoff;

    return 0;
}

struct page *vm_object_find_page(struct vm_object *obj, uint64_t pgoff) {
    struct rb_node *node = obj->page_tree.rb_node;

    while (node) {
        struct page_node *this = container_of(node, struct page_node, rb);
        if (pgoff < this->pgoff)
            node = node->rb_left;
        else if (pgoff > this->pgoff)
            node = node->rb_right;
        else
            return this->page;
    }

    return NULL;
}

void vm_object_remove_page(struct vm_object *obj, uint64_t pgoff) {
    struct rb_node *node = obj->page_tree.rb_node;

    while (node) {
        struct page_node *this = rb_entry(node, struct page_node, rb);
        if (pgoff < this->pgoff)
            node = node->rb_left;
        else if (pgoff > this->pgoff)
            node = node->rb_right;
        else {
            rb_erase(&this->rb, &obj->page_tree);
            this->page->mapping = NULL;
            kfree(this);
            return;
        }
    }
}

/* Anonymous Object Implementation */

static int anon_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
    irq_flags_t flags = spinlock_lock_irqsave(&obj->lock);
    
    struct page *page = vm_object_find_page(obj, vmf->pgoff);
    if (page) {
        get_page(page);
        vmf->page = page;
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return 0;
    }

    /* Allocate a new page */
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return VM_FAULT_OOM;
    }

    page = phys_to_page(phys);
    memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);

    if (vm_object_add_page(obj, vmf->pgoff, page) < 0) {
        pmm_free_page(phys);
        page = vm_object_find_page(obj, vmf->pgoff);
        if (page) {
            get_page(page);
            vmf->page = page;
            spinlock_unlock_irqrestore(&obj->lock, flags);
            return 0;
        }
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return VM_FAULT_SIGBUS;
    }

    /* Handle Reverse Mapping (RMAP) */
    if (vma->anon_vma) {
        folio_add_anon_rmap(page_folio(page), vma, vmf->address);
    }

    get_page(page);
    vmf->page = page;
    spinlock_unlock_irqrestore(&obj->lock, flags);
    return 0;
}

static int device_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
    /* 
     * Device memory doesn't have 'struct page' entries in many cases.
     * We map the physical address directly.
     */
    uint64_t phys = obj->phys_addr + (vmf->pgoff << PAGE_SHIFT);
    uint64_t pte_flags = PTE_PRESENT;
    
    if (vma->vm_flags & VM_USER) pte_flags |= PTE_USER;
    if (vma->vm_flags & VM_WRITE) pte_flags |= PTE_RW;
    if (!(vma->vm_flags & VM_EXEC)) pte_flags |= PTE_NX;

    /* Handle Cache Attributes (UBC feature) */
    if (vma->vm_flags & VM_CACHE_WC) pte_flags |= VMM_CACHE_WC;
    else if (vma->vm_flags & VM_CACHE_UC) pte_flags |= VMM_CACHE_UC;
    else if (vma->vm_flags & VM_CACHE_WT) pte_flags |= VMM_CACHE_WT;

    vmm_map_page((uint64_t) vma->vm_mm->pml4, vmf->address, phys, pte_flags);

    return VM_FAULT_COMPLETED; /* Signal that mapping is already done */
}

static const struct vm_object_operations anon_obj_ops = {
    .fault = anon_obj_fault,
};

static const struct vm_object_operations device_obj_ops = {
    .fault = device_obj_fault,
};

struct vm_object *vm_object_anon_create(size_t size) {
    struct vm_object *obj = vm_object_alloc(VM_OBJECT_ANON);
    if (!obj) return NULL;

    obj->size = size;
    obj->ops = &anon_obj_ops;
    return obj;
}

struct vm_object *vm_object_device_create(uint64_t phys_addr, size_t size) {
    struct vm_object *obj = vm_object_alloc(VM_OBJECT_DEVICE);
    if (!obj) return NULL;

    obj->phys_addr = phys_addr;
    obj->size = size;
    obj->ops = &device_obj_ops;
    return obj;
}
