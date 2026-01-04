/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vm_object.c
 * @brief Virtual Memory Object management
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

#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <kernel/errno.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vm_object.h>

#define page_to_phys(page) PFN_TO_PHYS(page_to_pfn(page))

/* Forward declarations for RMAP (defined in memory.c) */
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address);


/* Page Tree Management */
struct page_node {
  struct rb_node rb;
  uint64_t pgoff;
  struct page *page;
  int order;
};

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
    struct page_node *pnode = container_of(node, struct page_node, rb);

    rb_erase(&pnode->rb, &obj->page_tree);

    if (pnode->page) {
      put_page(pnode->page);
    }

    kfree(pnode);
    node = next;
  }

  if (obj->ops && obj->ops->free) {
    obj->ops->free(obj);
  }

  if (obj->backing_object) {
    vm_object_put(obj->backing_object);
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
  node->order = page->order;

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
    
    // 1. Bounds check
    if (vmf->pgoff >= (obj->size >> PAGE_SHIFT)) {
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return VM_FAULT_SIGSEGV;
    }

    // 2. Check if page already exists
    struct page *page = vm_object_find_page(obj, vmf->pgoff);
    if (page) {
        get_page(page);
        vmf->page = page;
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return 0;
    }

    spinlock_unlock_irqrestore(&obj->lock, flags);

    /* 3. Prepare new page (ALLOCATION OUTSIDE LOCK) */
    struct folio *folio = NULL;
    bool is_huge = false;

    int nid = vma->preferred_node;
    if (nid == -1 && vma->vm_mm) nid = vma->vm_mm->preferred_node;

    // Try Huge Page (2MB) if aligned and supported
    if ((vma->vm_flags & VM_HUGE) && (vmf->pgoff % 512 == 0) && (vmf->pgoff + 512 <= (obj->size >> PAGE_SHIFT))) {
        folio = alloc_pages_node(nid, GFP_KERNEL, 9);
        if (folio) {
            memset(pmm_phys_to_virt(folio_to_phys(folio)), 0, VMM_PAGE_SIZE_2M);
            is_huge = true;
        }
    }

    // Fallback to 4KB if huge failed or wasn't applicable
    if (!folio) {
        folio = alloc_pages_node(nid, GFP_KERNEL, 0);
        if (!folio) return VM_FAULT_OOM;
        memset(pmm_phys_to_virt(folio_to_phys(folio)), 0, PAGE_SIZE);
        is_huge = false;
    }

    /* 4. Try to insert the new page (RACING RE-CHECK) */
    flags = spinlock_lock_irqsave(&obj->lock);
    page = vm_object_find_page(obj, vmf->pgoff);
    
    if (page) {
        // Someone else beat us to it, free our page and use theirs
        spinlock_unlock_irqrestore(&obj->lock, flags);
        folio_put(folio);
        
        flags = spinlock_lock_irqsave(&obj->lock);
        page = vm_object_find_page(obj, vmf->pgoff);
        if (!page) { // Should not happen but safety first
             spinlock_unlock_irqrestore(&obj->lock, flags);
             return VM_FAULT_RETRY;
        }
        get_page(page);
        vmf->page = page;
        spinlock_unlock_irqrestore(&obj->lock, flags);
        return 0;
    }

    // Insert our page
    if (vm_object_add_page(obj, vmf->pgoff, &folio->page) < 0) {
        spinlock_unlock_irqrestore(&obj->lock, flags);
        folio_put(folio);
        return VM_FAULT_SIGBUS;
    }

    /* Handle Reverse Mapping (RMAP) */
    if (vma->anon_vma) {
        folio_add_anon_rmap(folio, vma, vmf->address);
    }

    get_page(&folio->page);
    vmf->page = &folio->page;
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

  vmm_map_page(vma->vm_mm, vmf->address, phys, pte_flags);

  return VM_FAULT_COMPLETED; /* Signal that mapping is already done */
}

static const struct vm_object_operations anon_obj_ops = {
  .fault = anon_obj_fault,
};

static const struct vm_object_operations device_obj_ops = {
  .fault = device_obj_fault,
};

static int shadow_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  spinlock_lock(&obj->lock);

  /* 1. Check if the page is already in the shadow object */
  struct page *page = vm_object_find_page(obj, vmf->pgoff);
  if (page) {
    get_page(page);
    vmf->page = page;
    spinlock_unlock(&obj->lock);
    return 0;
  }

  /* 2. Page not in shadow, check backing object */
  struct vm_object *backing = obj->backing_object;
  if (!backing) {
    spinlock_unlock(&obj->lock);
    return VM_FAULT_SIGSEGV;
  }

  uint64_t backing_pgoff = vmf->pgoff + (obj->shadow_offset >> PAGE_SHIFT);
  struct vm_fault backing_vmf = *vmf;
  backing_vmf.pgoff = backing_pgoff;

  /*
   * If it's a read fault, we can just return the page from the backing object
   * WITHOUT copying it (mapping it read-only).
   * BUT: To simplify for now, and since we might change the PTE to read-only
   * at the arch level, let's just use the backing object's fault handler.
   */
  int ret = backing->ops->fault(backing, vma, &backing_vmf);
  if (ret != 0) {
    spinlock_unlock(&obj->lock);
    return ret;
  }

  page = backing_vmf.page;

  /* 3. If it's a WRITE fault, we MUST "promote" (copy) the page to the shadow object */
  if (vmf->flags & FAULT_FLAG_WRITE) {
    int nid = vma->preferred_node;
    if (nid == -1 && vma->vm_mm) nid = vma->vm_mm->preferred_node;

    struct folio *new_folio = alloc_pages_node(nid, GFP_KERNEL, 0);
    if (!new_folio) {
      put_page(page);
      spinlock_unlock(&obj->lock);
      return VM_FAULT_OOM;
    }

    struct page *new_page = &new_folio->page;
    uint64_t phys = folio_to_phys(new_folio);
    void *src = pmm_phys_to_virt(page_to_phys(page));
    void *dst = pmm_phys_to_virt(phys);
    memcpy(dst, src, PAGE_SIZE);

    put_page(page); /* Release backing page reference */

    if (vm_object_add_page(obj, vmf->pgoff, new_page) < 0) {
      pmm_free_page(phys);
      spinlock_unlock(&obj->lock);
      return VM_FAULT_SIGBUS;
    }

    page = new_page;
    get_page(page);
  }

  vmf->page = page;
  spinlock_unlock(&obj->lock);
  return 0;
}

static const struct vm_object_operations shadow_obj_ops = {
  .fault = shadow_obj_fault,
};

struct vm_object *vm_object_shadow_create(struct vm_object *backing, uint64_t offset, size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_ANON); /* Shadow is essentially anonymous */
  if (!obj) return NULL;

  obj->backing_object = backing;
  if (backing) vm_object_get(backing);

  obj->shadow_offset = offset;
  obj->size = size;
  obj->ops = &shadow_obj_ops;

  return obj;
}

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

int vm_object_cow_prepare(struct vm_area_struct *vma, struct vm_area_struct *new_vma) {
  struct vm_object *old_obj = vma->vm_obj;
  if (!old_obj) return 0;

  /*
   * Create shadow objects for both.
   * They will both point to the same original backing object.
   */
  struct vm_object *shadow_parent = vm_object_shadow_create(old_obj, vma->vm_pgoff << PAGE_SHIFT, vma_size(vma));
  struct vm_object *shadow_child = vm_object_shadow_create(old_obj, vma->vm_pgoff << PAGE_SHIFT, vma_size(vma));

  if (!shadow_parent || !shadow_child) {
    if (shadow_parent) vm_object_put(shadow_parent);
    if (shadow_child) vm_object_put(shadow_child);
    return -ENOMEM;
  }

  /* Update VMAs */
  vma->vm_obj = shadow_parent;
  vma->vm_pgoff = 0; /* Shadow handles the offset now */

  new_vma->vm_obj = shadow_child;
  new_vma->vm_pgoff = 0;

  vm_object_put(old_obj);
  return 0;
}
