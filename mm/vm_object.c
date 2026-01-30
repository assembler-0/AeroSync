/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vm_object.c
 * @brief Virtual Memory Object management
 * @copyright (C) 2025-2026 assembler-0
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
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/zmm.h>

/* Page Tree Management - Now using embedded obj_node in struct page for O(1) node allocation */

struct vm_object *vm_object_alloc(vm_object_type_t type) {
  struct vm_object *obj = kmalloc(sizeof(struct vm_object));
  if (!obj) return nullptr;

  memset(obj, 0, sizeof(struct vm_object));
  obj->type = type;
  xa_init(&obj->page_tree);
  rwsem_init(&obj->lock);
  INIT_LIST_HEAD(&obj->i_mmap);
  INIT_LIST_HEAD(&obj->dirty_list);
  obj->flags = 0;
  atomic_set(&obj->refcount, 1);

  /* Shadow chain management defaults */
  obj->shadow_depth = 0;
#ifdef CONFIG_MM_SHADOW_DEPTH_LIMIT
  obj->collapse_threshold = CONFIG_MM_SHADOW_DEPTH_LIMIT;
#else
  obj->collapse_threshold = 8;
#endif
  atomic_set(&obj->shadow_children, 0);

  /* NUMA */
  obj->preferred_node = -1; /* No preference */

  /* Readahead */
  obj->cluster_shift = 4; /* 16 pages default */

  /* Statistics */
  atomic_long_set(&obj->nr_pages, 0);
  atomic_long_set(&obj->nr_swap, 0);

  return obj;
}

void vm_object_free(struct vm_object *obj) {
  if (!obj) return;

  unsigned long index;
  struct folio *folio;

  /* Use efficient XArray iterator for cleanup */
  xa_for_each(&obj->page_tree, index, folio) {
    xa_erase(&obj->page_tree, index);
    if (folio && !xa_is_err(folio)) {
      if ((uintptr_t) folio & 0x1) {
#ifdef CONFIG_MM_ZMM
        zmm_free_handle((zmm_handle_t) ((uintptr_t) folio & ~0x1));
#endif
      } else {
        folio->mapping = nullptr;
        folio_put(folio);
      }
    }
  }

  xa_destroy(&obj->page_tree);

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
  /*
   * xa_store returns 0 on success, or an error code.
   * It handles tree growth and synchronization.
   */
  if (xa_store(&obj->page_tree, pgoff, folio, GFP_KERNEL))
    return -ENOMEM;

  folio->index = pgoff;

  /* Standard RMAP linkage for non-anonymous folios */
  folio_add_file_rmap(folio, obj, pgoff);

  return 0;
}

struct folio *vm_object_find_folio(struct vm_object *obj, uint64_t pgoff) {
  void *entry = xa_load(&obj->page_tree, pgoff);
  if (xa_is_err(entry) || !entry) return nullptr;
  return (struct folio *) entry;
}

void vm_object_remove_folio(struct vm_object *obj, uint64_t pgoff) {
  struct folio *folio = xa_erase(&obj->page_tree, pgoff);
  if (folio && !xa_is_err(folio)) {
    /* 1. Unmap from all virtual address spaces using reverse mapping */
    try_to_unmap_folio(folio, nullptr);

    /* 2. Dissociate from this object */
    folio->mapping = nullptr;

    /* 3. Release the reference held by the object's page tree */
    folio_put(folio);
  }
}

int vm_object_add_page(struct vm_object *obj, uint64_t pgoff, struct page *page) {
  return vm_object_add_folio(obj, pgoff, page_folio(page));
}

struct page *vm_object_find_page(struct vm_object *obj, uint64_t pgoff) {
  struct folio *f = vm_object_find_folio(obj, pgoff);
  return f ? &f->page : nullptr;
}

void vm_object_remove_page(struct vm_object *obj, uint64_t pgoff) {
  vm_object_remove_folio(obj, pgoff);
}

#include <mm/zmm.h>
#include <mm/swap.h>
#include <mm/workingset.h>

/*
 * XArray entry encoding for vm_object page_tree:
 *
 * Bits [1:0] determine the entry type:
 *   0b00 = Real folio pointer (8-byte aligned)
 *   0b01 = ZMM compressed handle
 *   0b10 = Swap entry
 *   0b11 = Workingset shadow entry
 */
#define ENTRY_TYPE_MASK     0x3
#define ENTRY_TYPE_FOLIO    0x0
#define ENTRY_TYPE_ZMM      0x1
#define ENTRY_TYPE_SWAP     0x2
#define ENTRY_TYPE_SHADOW   0x3

static inline int xa_entry_type(void *entry) {
  return (uintptr_t) entry & ENTRY_TYPE_MASK;
}

/* Anonymous Object Implementation */
static int anon_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  /*
   * SPECULATIVE PATH:
   * If the folio exists, we can handle it completely locklessly (with RCU).
   * If it doesn't, we can still proceed if we can take the object lock without blocking.
   */
  struct folio *folio = vm_object_find_folio(obj, vmf->pgoff);
  if (vmf->flags & FAULT_FLAG_SPECULATIVE) {
    if (!down_read_trylock(&obj->lock)) return VM_FAULT_RETRY;
    if (folio) {
      folio_get(folio);
      vmf->folio = folio;
      vmf->prot = vma->vm_page_prot;
      up_read(&obj->lock);
      return 0;
    }
    up_read(&obj->lock);

    /*
     * If not found, we could try to allocate, but for now let's only
     * allow speculative hits on existing pages.
     * TODO: Allow speculative allocation if vma_lock is held.
     */
    return VM_FAULT_RETRY;
  }

  /* Ensure RMAP is ready */
  if (unlikely(anon_vma_prepare(vma))) return VM_FAULT_OOM;

  down_read(&obj->lock);

  // 1. Bounds check
  if (vmf->pgoff >= (obj->size >> PAGE_SHIFT)) {
    up_read(&obj->lock);
    return VM_FAULT_SIGSEGV;
  }

  // 2. Check if folio already exists
  void *entry = xa_load(&obj->page_tree, vmf->pgoff);

  if (entry && !xa_is_err(entry)) {
    int entry_type = xa_entry_type(entry);

    switch (entry_type) {
      case ENTRY_TYPE_ZMM: {
#ifdef CONFIG_MM_ZMM
        zmm_handle_t handle = (zmm_handle_t) ((uintptr_t) entry & ~ENTRY_TYPE_MASK);

        up_read(&obj->lock);
        struct folio *new_folio = alloc_pages(GFP_KERNEL, 0);
        if (!new_folio) return VM_FAULT_OOM;

        if (zmm_decompress_to_folio(handle, new_folio) != 0) {
          folio_put(new_folio);
          return VM_FAULT_SIGBUS;
        }

        down_write(&obj->lock);
        void *recheck = xa_load(&obj->page_tree, vmf->pgoff);
        if (recheck != entry) {
          up_write(&obj->lock);
          folio_put(new_folio);
          return VM_FAULT_RETRY;
        }

        xa_store(&obj->page_tree, vmf->pgoff, new_folio, GFP_KERNEL);
        zmm_free_handle(handle);
        atomic_long_inc(&obj->nr_pages);

        folio_get(new_folio);
        vmf->folio = new_folio;
        vmf->prot = vma->vm_page_prot;

        up_write(&obj->lock);
        if (vma->anon_vma) folio_add_anon_rmap(new_folio, vma, vmf->address);
        return 0;
#else
        return VM_FAULT_SIGBUS;
#endif
      }

      case ENTRY_TYPE_SWAP: {
#ifdef CONFIG_MM_SWAP
        /* Decode swap entry */
        swp_entry_t swap_entry;
        swap_entry.val = ((uintptr_t) entry) >> 2;

        up_read(&obj->lock);

        /* Read page from swap (with readahead) */
        struct folio *new_folio = swap_cluster_readahead(swap_entry, GFP_KERNEL);
        if (!new_folio) {
          new_folio = swap_readpage(swap_entry);
        }
        if (!new_folio) return VM_FAULT_OOM;

#ifdef CONFIG_MM_WORKINGSET
        /* Check for refault - activate if recently evicted */
        workingset_refault(new_folio, entry);
#endif

        down_write(&obj->lock);
        void *recheck = xa_load(&obj->page_tree, vmf->pgoff);
        if (recheck != entry) {
          up_write(&obj->lock);
          folio_put(new_folio);
          return VM_FAULT_RETRY;
        }

        /* Replace swap entry with folio */
        xa_store(&obj->page_tree, vmf->pgoff, new_folio, GFP_KERNEL);
        atomic_long_inc(&obj->nr_pages);
        atomic_long_dec(&obj->nr_swap);

        /* Free the swap slot */
        swap_free(swap_entry);

        /* Remove from swap cache */
        delete_from_swap_cache(new_folio);

        folio_get(new_folio);
        vmf->folio = new_folio;
        vmf->prot = vma->vm_page_prot;

        up_write(&obj->lock);
        if (vma->anon_vma) folio_add_anon_rmap(new_folio, vma, vmf->address);
        return 0;
#else
        return VM_FAULT_SIGBUS;
#endif
      }

      case ENTRY_TYPE_SHADOW: {
        /* Shadow entry indicates the page was evicted - treat as missing */
#ifdef CONFIG_MM_WORKINGSET
        /* Store shadow for later refault detection */
        void *shadow = entry;
#endif
        up_read(&obj->lock);
        goto allocate_new_with_shadow;
      }

      default: /* ENTRY_TYPE_FOLIO */
        folio = (struct folio *) entry;

        /* COW for Zero Page */
        if ((vmf->flags & FAULT_FLAG_WRITE) && folio_to_phys(folio) == empty_zero_page) {
          goto allocate_new;
        }

        folio_get(folio);
        vmf->folio = folio;
        vmf->prot = vma->vm_page_prot;
        if (folio_to_phys(folio) == empty_zero_page) vmf->prot &= ~PTE_RW;

        up_read(&obj->lock);
        return 0;
    }
  }

  /* Zero-Page Optimization for initial reads */
  if (!(vmf->flags & FAULT_FLAG_WRITE)) {
    struct folio *zf = (struct folio *) &mem_map[PHYS_TO_PFN(empty_zero_page)];
    vmf->folio = zf;
    vmf->prot = vma->vm_page_prot & ~PTE_RW;

    down_write(&obj->lock);
    if (!xa_load(&obj->page_tree, vmf->pgoff)) {
      xa_store(&obj->page_tree, vmf->pgoff, zf, GFP_KERNEL);
    }
    up_write(&obj->lock);
    return 0;
  }

allocate_new:
  up_read(&obj->lock);

allocate_new_with_shadow:
  ; /* Label requires statement */

  /* 3. Prepare new folio (ALLOCATION OUTSIDE LOCK) */
  folio = nullptr;
  int nid = vma->preferred_node;
  if (nid == -1 && obj->preferred_node != -1) nid = obj->preferred_node;
  if (nid == -1 && vma->vm_mm) nid = vma->vm_mm->preferred_node;
  if (nid == -1) nid = this_node();

  /*
   * Opportunistic THP:
   * 1. Must be aligned to 2MB (512 pages).
   * 2. Must not have VM_NOHUGEPAGE set.
   * 3. Must fit in object bounds.
   */
  bool thp_eligible = (vmf->pgoff % 512 == 0) &&
                      (vmf->pgoff + 512 <= (obj->size >> PAGE_SHIFT)) &&
                      !(vma->vm_flags & VM_NOHUGEPAGE);

  if (thp_eligible) {
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

  atomic_long_inc(&obj->nr_pages);
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

static int file_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *folio;
  int ret;

  /* 1. Check if the page is already in the cache */
  down_read(&obj->lock);
  folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma->vm_page_prot;

    /* If it's a private mapping, we must drop write permission to trigger COW later */
    if (!(vma->vm_flags & VM_SHARED)) {
      vmf->prot &= ~PTE_RW;
    }

    up_read(&obj->lock);
    return 0;
  }
  up_read(&obj->lock);

  /* 2. Bounds check */
  if (vmf->pgoff >= (obj->size >> PAGE_SHIFT)) return VM_FAULT_SIGSEGV;

  /* 3. Allocate a new folio */
  int nid = vma->preferred_node;
  if (nid == -1) nid = this_node();

  folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!folio) return VM_FAULT_OOM;

  /* 4. Read from the file */
  if (obj->ops && obj->ops->read_folio) {
    ret = obj->ops->read_folio(obj, folio);
    if (ret != 0) {
      folio_put(folio);
      return VM_FAULT_SIGBUS;
    }
  } else {
    /* Generic case: just zero the page if no read_folio (simulates a hole) */
    memset(folio_address(folio), 0, PAGE_SIZE);
  }

  /* 5. Insert into the page tree (RACING CHECK) */
  down_write(&obj->lock);
  struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);
  if (existing) {
    up_write(&obj->lock);
    folio_put(folio);
    folio_get(existing);
    vmf->folio = existing;
    vmf->prot = vma->vm_page_prot;
    if (!(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;
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
  if (!(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;

  up_write(&obj->lock);

  /* Link to RMAP for reclamation */
  folio_add_file_rmap(folio, obj, vmf->pgoff);

  return 0;
}

static const struct vm_object_operations file_obj_ops = {
  .fault = file_obj_fault,
};

struct vm_object *vm_object_file_create(struct file *file, size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_FILE);
  if (!obj) return nullptr;

  obj->file = file;
  obj->size = size;
  obj->ops = &file_obj_ops;
  return obj;
}

/**
 * vm_object_shadow_depth - Get the depth of a shadow chain
 * @obj: The object to check
 *
 * Returns the depth of the shadow chain (0 for non-shadow objects).
 */
int vm_object_shadow_depth(struct vm_object *obj) {
  int depth = 0;
  while (obj && obj->backing_object) {
    depth++;
    obj = obj->backing_object;
  }
  return depth;
}

/**
 * vm_object_collapse - Collapses a shadow chain.
 * if the backing object is no longer shared. This reduces shadow chain depth
 * and improves performance of page lookups.
 *
 * Collapse conditions:
 *   1. backing_object exists
 *   2. backing_object has refcount == 1 (only us)
 *   3. backing_object is anonymous
 *   4. We can acquire both locks without blocking
 */
void vm_object_collapse(struct vm_object *obj) {
  struct vm_object *backing;
  int depth_limit = 100; /* Prevent infinite loops */

  while (depth_limit--) {
    backing = obj->backing_object;
    if (!backing) return;

    /*
     * Optimization: If the backing object has only 1 reference (us),
     * we can merge its pages into our tree and bypass it.
     */
    if (backing->type != VM_OBJECT_ANON || atomic_read(&backing->refcount) != 1) {
      return;
    }

    /* Check shadow children count - if backing has other shadows, don't collapse */
    if (atomic_read(&backing->shadow_children) > 1) {
      return;
    }

    /* LOCK ORDER: Child -> Parent */
    if (!down_write_trylock(&obj->lock)) return;
    if (!down_write_trylock(&backing->lock)) {
      up_write(&obj->lock);
      return;
    }

    /* Re-check refcount under lock */
    if (atomic_read(&backing->refcount) != 1) {
      up_write(&backing->lock);
      up_write(&obj->lock);
      return;
    }

    /* Mark as collapsing to prevent races */
    obj->flags |= VM_OBJECT_COLLAPSING;

    unsigned long backing_idx;
    struct folio *folio;

    /*
     * Efficiently migrate pages from backing object using XArray iterator.
     * We use xa_for_each to skip holes and process existing folios.
     */
    xa_for_each(&backing->page_tree, backing_idx, folio) {
      if (!folio || xa_is_err(folio)) continue;

      uint64_t backing_offset = backing_idx << PAGE_SHIFT;

      /* Boundary checks relative to shadow window */
      if (backing_offset < obj->shadow_offset) {
        xa_erase(&backing->page_tree, backing_idx);
        if (!((uintptr_t) folio & 0x3)) {
          /* Real folio, not shadow/ZMM */
          folio->mapping = nullptr;
          folio_put(folio);
          atomic_long_dec(&backing->nr_pages);
        }
        continue;
      }

      uint64_t obj_pgoff = (backing_offset - obj->shadow_offset) >> PAGE_SHIFT;
      if (obj_pgoff >= (obj->size >> PAGE_SHIFT)) {
        xa_erase(&backing->page_tree, backing_idx);
        if (!((uintptr_t) folio & 0x3)) {
          folio->mapping = nullptr;
          folio_put(folio);
          atomic_long_dec(&backing->nr_pages);
        }
        continue;
      }

      /*
       * If we don't have this page yet, take it from backing.
       * If we do have it, it's already 'shadowed', so the backing version is stale.
       */
      if (!xa_load(&obj->page_tree, obj_pgoff)) {
        xa_erase(&backing->page_tree, backing_idx);

        /* Update mapping to point to the new owner */
        if (!((uintptr_t) folio & 0x3)) {
          folio->mapping = (void *) obj;
          folio->index = obj_pgoff;
          xa_store(&obj->page_tree, obj_pgoff, folio, GFP_ATOMIC);
          atomic_long_inc(&obj->nr_pages);
          atomic_long_dec(&backing->nr_pages);
        } else {
          /* ZMM or shadow entry - copy as-is */
          xa_store(&obj->page_tree, obj_pgoff, folio, GFP_ATOMIC);
        }
      } else {
        xa_erase(&backing->page_tree, backing_idx);
        if (!((uintptr_t) folio & 0x3)) {
          folio->mapping = nullptr;
          folio_put(folio);
          atomic_long_dec(&backing->nr_pages);
        }
      }
    }

    /* Link to the next object in the chain */
    struct vm_object *new_backing = backing->backing_object;
    if (new_backing) {
      vm_object_get(new_backing);
      atomic_inc(&new_backing->shadow_children);
    }

    /* Update our shadow chain state */
    obj->backing_object = new_backing;
    obj->shadow_offset += backing->shadow_offset;
    obj->shadow_depth = new_backing ? new_backing->shadow_depth + 1 : 0;
    obj->flags &= ~VM_OBJECT_COLLAPSING;

    /* Decrement shadow children count on old backing */
    atomic_dec(&backing->shadow_children);

    up_write(&backing->lock);
    up_write(&obj->lock);

    /*
     * Backing is now orphaned. We must release it.
     * Since we were the only reference, this will trigger its destruction.
     */
    vm_object_put(backing);

    /* Continue collapsing if the new backing object is also collapsible */
  }
}

#ifdef CONFIG_MM_SHADOW_ASYNC_COLLAPSE
#include <aerosync/workqueue.h>

struct collapse_work {
  struct work_struct work;
  struct vm_object *obj;
};

static void collapse_work_fn(struct work_struct *work) {
  struct collapse_work *cw = container_of(work, struct collapse_work, work);
  vm_object_collapse(cw->obj);
  vm_object_put(cw->obj); /* Release ref taken when queueing */
  kfree(cw);
}

/**
 * vm_object_try_collapse_async - Queue async collapse if depth exceeds threshold
 * @obj: The shadow object to potentially collapse
 *
 * Returns 1 if collapse was queued, 0 otherwise.
 */
int vm_object_try_collapse_async(struct vm_object *obj) {
  if (!obj || !obj->backing_object)
    return 0;

  if (obj->shadow_depth < obj->collapse_threshold)
    return 0;

  /* Only collapse if backing has refcount == 1 */
  if (atomic_read(&obj->backing_object->refcount) != 1)
    return 0;

  struct collapse_work *cw = kmalloc(sizeof(*cw));
  if (!cw) {
    /* Fall back to synchronous collapse */
    vm_object_collapse(obj);
    return 1;
  }

  vm_object_get(obj); /* Keep obj alive until work completes */
  cw->obj = obj;
  INIT_WORK(&cw->work, collapse_work_fn);
  schedule_work(&cw->work);

  return 1;
}
#else
int vm_object_try_collapse_async(struct vm_object *obj) {
  /* Synchronous fallback */
  vm_object_collapse(obj);
  return 1;
}
#endif


static int shadow_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *new_folio = nullptr;
  struct folio *folio;
  int ret;

  /*
   * SPECULATIVE PATH:
   * Only support read faults on existing pages in the shadow chain.
   * Write faults or missing pages require the slow path due to COW/Collapsing complexity.
   */
  if (vmf->flags & FAULT_FLAG_SPECULATIVE) {
    if (vmf->flags & FAULT_FLAG_WRITE) return VM_FAULT_RETRY;

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
      return VM_FAULT_RETRY;
    }

    /* Recurse into backing object speculatively */
    uint64_t backing_pgoff = vmf->pgoff + (obj->shadow_offset >> PAGE_SHIFT);
    struct vm_fault backing_vmf = *vmf;
    backing_vmf.pgoff = backing_pgoff;

    /* We already checked FAULT_FLAG_WRITE above, so this is a read */
    ret = backing->ops->fault(backing, vma, &backing_vmf);
    up_read(&obj->lock);

    if (ret == 0) {
      vmf->folio = backing_vmf.folio;
      vmf->prot = vma->vm_page_prot & ~PTE_RW;
    }
    return ret;
  }

  /*
   * Shadow chain collapse:
   * If depth exceeds threshold, try async collapse.
   * Otherwise, do opportunistic sync collapse.
   */
#ifdef CONFIG_MM_SHADOW_ASYNC_COLLAPSE
  if (obj->shadow_depth >= obj->collapse_threshold) {
    vm_object_try_collapse_async(obj);
  } else {
    vm_object_collapse(obj);
  }
#else
  vm_object_collapse(obj);
#endif

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
    if (nid == -1 && obj->preferred_node != -1) nid = obj->preferred_node;
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

    atomic_long_inc(&obj->nr_pages);
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
  if (!obj) return nullptr;

  obj->backing_object = backing;
  if (backing) {
    vm_object_get(backing);
    atomic_inc(&backing->shadow_children);

    /* Track shadow chain depth */
    obj->shadow_depth = backing->shadow_depth + 1;

    /* Inherit collapse threshold from parent */
    obj->collapse_threshold = backing->collapse_threshold;

    /* Inherit NUMA preference if not set */
    if (obj->preferred_node == -1)
      obj->preferred_node = backing->preferred_node;
  }

  obj->shadow_offset = offset;
  obj->size = size;
  obj->flags |= VM_OBJECT_SHADOW;
  obj->ops = &shadow_obj_ops;

  return obj;
}

struct vm_object *vm_object_anon_create(size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_ANON);
  if (!obj) return nullptr;

  obj->size = size;
  obj->ops = &anon_obj_ops;
  return obj;
}

struct vm_object *vm_object_device_create(uint64_t phys_addr, size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_DEVICE);
  if (!obj) return nullptr;

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
