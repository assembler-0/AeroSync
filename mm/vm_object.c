/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vm_object.c
 * @brief Virtual Memory Object management
 * @copyright (C) 2025 assembler-0
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

#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/errno.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <mm/page.h>

#define page_to_phys(page) PFN_TO_PHYS(page_to_pfn(page))

/* Forward declarations for RMAP (defined in memory.c) */
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address);

/* Page Tree Management - Now using embedded obj_node in struct page for O(1) node allocation */

struct vm_object *vm_object_alloc(vm_object_type_t type) {
  struct vm_object *obj = kmalloc(sizeof(struct vm_object));
  if (!obj) return NULL;

  memset(obj, 0, sizeof(struct vm_object));
  obj->type = type;
  obj->page_tree = RB_ROOT;
  rwsem_init(&obj->lock);
  INIT_LIST_HEAD(&obj->i_mmap);
  INIT_LIST_HEAD(&obj->dirty_list);
  obj->flags = 0;
  atomic_set(&obj->refcount, 1);

  return obj;
}

void vm_object_free(struct vm_object *obj) {
  if (!obj) return;

  /* Free all pages in the tree - SAFE iteration by repeatedly pulling first node */
  struct rb_node *node;
  while ((node = rb_first(&obj->page_tree))) {
    struct folio *folio = rb_entry(node, struct folio, rb_node);

    rb_erase(&folio->rb_node, &obj->page_tree);
    folio->mapping = NULL;
    folio_put(folio);
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

int vm_object_add_folio(struct vm_object *obj, uint64_t pgoff, struct folio *folio) {
  struct rb_node **new = &obj->page_tree.rb_node, *parent = NULL;

  while (*new) {
    struct folio *this = rb_entry(*new, struct folio, rb_node);
    parent = *new;
    if (pgoff < this->index)
      new = &((*new)->rb_left);
    else if (pgoff > this->index)
      new = &((*new)->rb_right);
    else
      return -EEXIST;
  }

  folio->index = pgoff;

  /* Standard RMAP linkage for non-anonymous folios */
  folio_add_file_rmap(folio, obj, pgoff);

  rb_link_node(&folio->rb_node, parent, new);
  rb_insert_color(&folio->rb_node, &obj->page_tree);

  return 0;
}

struct folio *vm_object_find_folio(struct vm_object *obj, uint64_t pgoff) {
  struct rb_node *node = obj->page_tree.rb_node;

  while (node) {
    struct folio *this = rb_entry(node, struct folio, rb_node);
    if (pgoff < this->index)
      node = node->rb_left;
    else if (pgoff > this->index)
      node = node->rb_right;
    else
      return this;
  }

  return NULL;
}

void vm_object_remove_folio(struct vm_object *obj, uint64_t pgoff) {
  struct folio *folio = vm_object_find_folio(obj, pgoff);
  if (folio) {
      /* 1. Unmap from all virtual address spaces using reverse mapping */
      try_to_unmap_folio(folio, NULL);
    /* 2. Remove from the object's page tree */
    rb_erase(&folio->rb_node, &obj->page_tree);

    /* 3. Dissociate from this object */
    folio->mapping = NULL;

    /* 4. Release the reference held by the object's page tree */
    folio_put(folio);
  }
}

int vm_object_add_page(struct vm_object *obj, uint64_t pgoff, struct page *page) {
  return vm_object_add_folio(obj, pgoff, page_folio(page));
}

struct page *vm_object_find_page(struct vm_object *obj, uint64_t pgoff) {
  struct folio *f = vm_object_find_folio(obj, pgoff);
  return f ? &f->page : NULL;
}

void vm_object_remove_page(struct vm_object *obj, uint64_t pgoff) {
  vm_object_remove_folio(obj, pgoff);
}

/* Anonymous Object Implementation */
static int anon_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  /* Ensure RMAP is ready */
  if (unlikely(anon_vma_prepare(vma))) return VM_FAULT_OOM;

  down_read(&obj->lock);

  // 1. Bounds check
  if (vmf->pgoff >= (obj->size >> PAGE_SHIFT)) {
    up_read(&obj->lock);
    return VM_FAULT_SIGSEGV;
  }

  // 2. Check if folio already exists
  struct folio *folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma->vm_page_prot;
    up_read(&obj->lock);
    return 0;
  }

  up_read(&obj->lock);

  /* 3. Prepare new folio (ALLOCATION OUTSIDE LOCK) */
  folio = NULL;
  int nid = vma->preferred_node;
  if (nid == -1 && vma->vm_mm) nid = vma->vm_mm->preferred_node;
  if (nid == -1) nid = this_node();

  // Try Huge Page (2MB) if aligned and supported
  if ((vma->vm_flags & VM_HUGE) && (vmf->pgoff % 512 == 0) && (vmf->pgoff + 512 <= (obj->size >> PAGE_SHIFT))) {
    folio = alloc_pages_node(nid, GFP_KERNEL, 9);
    if (folio) {
      memset(pmm_phys_to_virt(folio_to_phys(folio)), 0, VMM_PAGE_SIZE_2M);
    }
  }

  // Fallback to 4KB if huge failed or wasn't applicable
  if (!folio) {
    folio = alloc_pages_node(nid, GFP_KERNEL, 0);
    if (!folio) {
      return VM_FAULT_OOM;
    }
    memset(pmm_phys_to_virt(folio_to_phys(folio)), 0, PAGE_SIZE);
  }

  /* 4. Try to insert the new folio (RACING RE-CHECK) */
  down_write(&obj->lock);
  struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);

  if (existing) {
    up_write(&obj->lock);
    folio_put(folio);

    down_read(&obj->lock);
    existing = vm_object_find_folio(obj, vmf->pgoff);
    if (!existing) {
      up_read(&obj->lock);
      return VM_FAULT_RETRY;
    }
    folio_get(existing);
    vmf->folio = existing;
    vmf->prot = vma->vm_page_prot;
    up_read(&obj->lock);
    return 0;
  }

  if (vm_object_add_folio(obj, vmf->pgoff, folio) < 0) {
    up_write(&obj->lock);
    folio_put(folio);
    return VM_FAULT_SIGBUS;
  }

  folio_get(folio);
  vmf->folio = folio;
  vmf->prot = vma->vm_page_prot;
  up_write(&obj->lock);

  /* Handle Reverse Mapping (RMAP) - OUTSIDE object lock to avoid deadlock with LRU lock */
  if (vma->anon_vma) {
    folio_add_anon_rmap(folio, vma, vmf->address);
  }

  return 0;
}

static int device_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  uint64_t phys = obj->phys_addr + (vmf->pgoff << PAGE_SHIFT);
  vmf->prot = vma->vm_page_prot;
  vmm_map_page(vma->vm_mm, vmf->address, phys, vmf->prot);
  return VM_FAULT_COMPLETED;
}

static const struct vm_object_operations anon_obj_ops = {
  .fault = anon_obj_fault,
};

static const struct vm_object_operations device_obj_ops = {
  .fault = device_obj_fault,
};

/**
 * vm_object_collapse - Collapses a shadow chain.
 */
void vm_object_collapse(struct vm_object *obj) {
  struct vm_object *backing;

  while (1) {
    backing = obj->backing_object;
    if (!backing) return;

    /* Check if backing can be collapsed (only 1 ref, and it's us) */
    if (backing->type != VM_OBJECT_ANON || atomic_read(&backing->refcount) != 1) {
      return;
    }

    vm_object_get(backing);

    /* LOCK ORDER: Shadow (child) -> Backing (parent) to avoid ABBA deadlock with fault path */
    down_write(&obj->lock);
    down_write(&backing->lock);

    /* Re-check refcount under lock to avoid race */
    if (atomic_read(&backing->refcount) != 2) {
      up_write(&backing->lock);
      up_write(&obj->lock);
      vm_object_put(backing);
      return;
    }

    struct rb_node *node;
    while ((node = rb_first(&backing->page_tree))) {
      struct folio *folio = rb_entry(node, struct folio, rb_node);

      if (folio->index < (obj->shadow_offset >> PAGE_SHIFT)) {
        goto skip_page;
      }

      uint64_t obj_pgoff = folio->index - (obj->shadow_offset >> PAGE_SHIFT);
      if (obj_pgoff >= (obj->size >> PAGE_SHIFT)) {
        goto skip_page;
      }

      if (!vm_object_find_folio(obj, obj_pgoff)) {
        rb_erase(&folio->rb_node, &backing->page_tree);
        vm_object_add_folio(obj, obj_pgoff, folio);
        continue;
      }

    skip_page:
      rb_erase(&folio->rb_node, &backing->page_tree);
      folio->mapping = NULL;
      folio_put(folio);
    }

    struct vm_object *new_backing = backing->backing_object;
    if (new_backing) vm_object_get(new_backing);

    struct vm_object *old_backing = backing->backing_object;
    backing->backing_object = NULL;

    obj->backing_object = new_backing;
    obj->shadow_offset += backing->shadow_offset;

    up_write(&backing->lock);
    up_write(&obj->lock);

    vm_object_put(backing);
    vm_object_put(old_backing);
  }
}

static int shadow_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *new_folio = NULL;
  struct folio *folio;
  int ret;

  vm_object_collapse(obj);

  down_read(&obj->lock);

  folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma->vm_page_prot;
    up_read(&obj->lock);
    return 0;
  }

  struct vm_object *backing = obj->backing_object;
  if (!backing) {
    up_read(&obj->lock);
    return VM_FAULT_SIGSEGV;
  }

  up_read(&obj->lock);

  uint64_t backing_pgoff = vmf->pgoff + (obj->shadow_offset >> PAGE_SHIFT);
  struct vm_fault backing_vmf = *vmf;
  backing_vmf.pgoff = backing_pgoff;
  backing_vmf.flags &= ~FAULT_FLAG_WRITE;

  ret = backing->ops->fault(backing, vma, &backing_vmf);
  if (ret != 0) {
    return ret;
  }

  folio = backing_vmf.folio;

  if (vmf->flags & FAULT_FLAG_WRITE) {
    int nid = vma->preferred_node;
    if (nid == -1 && vma->vm_mm) nid = vma->vm_mm->preferred_node;
    if (nid == -1) nid = this_node();

    new_folio = alloc_pages_node(nid, GFP_KERNEL, 0);
    if (!new_folio) {
      folio_put(folio);
      return VM_FAULT_OOM;
    }

    void *src = pmm_phys_to_virt(folio_to_phys(folio));
    void *dst = pmm_phys_to_virt(folio_to_phys(new_folio));
    memcpy(dst, src, PAGE_SIZE);

    folio_put(folio);

    down_write(&obj->lock);

    struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);
    if (existing) {
      up_write(&obj->lock);
      folio_put(new_folio);
      folio_get(existing);
      vmf->folio = existing;
      vmf->prot = vma->vm_page_prot;
      return 0;
    }

    if (vm_object_add_folio(obj, vmf->pgoff, new_folio) < 0) {
      up_write(&obj->lock);
      folio_put(new_folio);
      return VM_FAULT_SIGBUS;
    }

    vmf->folio = new_folio;
    folio_get(vmf->folio);
    vmf->prot = vma->vm_page_prot;
    up_write(&obj->lock);

    if (vma->anon_vma) {
      folio_add_anon_rmap(new_folio, vma, vmf->address);
    }

    return 0;
  }

  vmf->folio = folio;
  vmf->prot = vma->vm_page_prot & ~PTE_RW;
  return 0;
}

static const struct vm_object_operations shadow_obj_ops = {
  .fault = shadow_obj_fault,
};

struct vm_object *vm_object_shadow_create(struct vm_object *backing, uint64_t offset, size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_ANON);
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

  struct vm_object *shadow_parent = vm_object_shadow_create(old_obj, vma->vm_pgoff << PAGE_SHIFT, vma_size(vma));
  struct vm_object *shadow_child = vm_object_shadow_create(old_obj, vma->vm_pgoff << PAGE_SHIFT, vma_size(vma));

  if (!shadow_parent || !shadow_child) {
    if (shadow_parent) vm_object_put(shadow_parent);
    if (shadow_child) vm_object_put(shadow_child);
    return -ENOMEM;
  }

  vma->vm_obj = shadow_parent;
  vma->vm_pgoff = 0;

  new_vma->vm_obj = shadow_child;
  new_vma->vm_pgoff = 0;

  vm_object_put(old_obj);
  return 0;
}
