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

#include <lib/math.h>
#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/errno.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/zmm.h>
#include <aerosync/resdomain.h>
#include <mm/swap.h>
#include <mm/workingset.h>
#include <aerosync/workqueue.h>

/* Page Tree Management - Now using embedded obj_node in struct page for O(1) node allocation */

atomic_long_t nr_shadow_objects = ATOMIC_LONG_INIT(0);

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

  /* Statistics */
  atomic_long_set(&obj->nr_pages, 0);
  atomic_long_set(&obj->nr_swap, 0);
  atomic_long_set(&obj->nr_dirty, 0);

  /* UBC / Writeback */
  obj->last_writeback = 0;
  obj->writeback_threshold = 128; /* 512KB dirty default */

  /* Readahead */
  obj->readahead.start = 0;
  obj->readahead.size = 0;
  obj->readahead.async_size = 0;
  obj->readahead.ra_pages = 32; /* 128KB max ra */
  obj->readahead.thrash_count = 0;

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

  if (obj->flags & VM_OBJECT_SHADOW) {
    atomic_long_dec(&nr_shadow_objects);
  }

  if (obj->backing_object) {
    atomic_dec(&obj->backing_object->shadow_children);
    vm_object_put(obj->backing_object);
  }

  kfree(obj);
}

void vm_object_get(struct vm_object *obj) {
  if (obj) atomic_inc(&obj->refcount);
}

void vm_object_put(struct vm_object *obj) {
  if (!obj) return;

  struct vm_object *backing = obj->backing_object;

  if (atomic_dec_and_test(&obj->refcount)) {
    vm_object_free(obj);

    /*
     * If the object we just freed was the last shadow of its backing object,
     * the backing object might now be eligible for collapsing into ITS child.
     */
    if (backing && atomic_read(&backing->refcount) == 1 &&
        (backing->flags & VM_OBJECT_SHADOW)) {
      /*
       * We need to find the object that points to 'backing'.
       * Since we don't have reverse pointers, we rely on the next fault
       * or a periodic scan.
       *
       * IMPROVEMENT: Proactive collapse if we can identify the child.
       */
    }
  } else if (backing && atomic_read(&obj->refcount) == 1 &&
             (obj->flags & VM_OBJECT_SHADOW)) {
    /*
     * Aggressive Overhaul: If this object is now uniquely owned (refcount == 1),
     * and it's a shadow, try to collapse it immediately.
     */
    vm_object_collapse(obj);
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
#ifdef CONFIG_MM_SPF
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
      vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
      up_read(&obj->lock);
      return 0;
    }
    up_read(&obj->lock);

    /*
     * If not found, we can attempt to allocate even during a speculative
     * fault, provided we can safely take the object lock and that the
     * VMA layout is stable (which is guaranteed if FAULT_FLAG_SPECULATIVE is set
     * and the caller holds vma->vm_lock).
     */
#ifdef CONFIG_MM_SPECULATIVE_ALLOC
    if (!down_write_trylock(&obj->lock)) return VM_FAULT_RETRY;

    /* Re-check existence under write lock */
    folio = vm_object_find_folio(obj, vmf->pgoff);
    if (folio) {
        folio_get(folio);
        vmf->folio = folio;
        vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
        up_write(&obj->lock);
        return 0;
    }

    /* Allocate and insert */
    int nid = vma ? vma->preferred_node : obj->preferred_node;
    if (nid == -1) nid = this_node();
    
    struct folio *new_folio = alloc_pages_node(nid, GFP_KERNEL | __GFP_NOWARN, 0);
    if (!new_folio) {
        up_write(&obj->lock);
        return VM_FAULT_OOM;
    }
    memset(folio_address(new_folio), 0, PAGE_SIZE);

    if (vm_object_add_folio(obj, vmf->pgoff, new_folio) < 0) {
        up_write(&obj->lock);
        folio_put(new_folio);
        return VM_FAULT_SIGBUS;
    }

    atomic_long_inc(&obj->nr_pages);
    folio_get(new_folio);
    vmf->folio = new_folio;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    up_write(&obj->lock);

    /* Link to RMAP - safe because we are in speculative path with stable VMA */
    if (vma && vma->anon_vma) {
        folio_add_anon_rmap(new_folio, vma, vmf->address);
    }

    return 0;
#else
    return VM_FAULT_RETRY;
#endif
  }
#endif

  /* Ensure RMAP is ready */
  if (unlikely(vma && anon_vma_prepare(vma))) return VM_FAULT_OOM;

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

    /* Sanity check: reject obviously invalid entries */
    if ((uintptr_t) entry == 0xadadadadadadadad ||
        (uintptr_t) entry == 0xadadadadadadadac) {
      up_read(&obj->lock);
      return VM_FAULT_SIGBUS;
    }

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
        vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);

        up_write(&obj->lock);
        if (vma && vma->anon_vma) folio_add_anon_rmap(new_folio, vma, vmf->address);
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
        vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);

        up_write(&obj->lock);
        if (vma && vma->anon_vma) folio_add_anon_rmap(new_folio, vma, vmf->address);
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
        vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
        if (folio_to_phys(folio) == empty_zero_page) vmf->prot &= ~PTE_RW;

        up_read(&obj->lock);
        return 0;
    }
  }

  /* Zero-Page Optimization for initial reads */
  if (!(vmf->flags & FAULT_FLAG_WRITE)) {
    struct folio *zf = (struct folio *) &mem_map[PHYS_TO_PFN(empty_zero_page)];
    vmf->folio = zf;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    vmf->prot &= ~PTE_RW;

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
  int nid = vma ? vma->preferred_node : obj->preferred_node;
  if (nid == -1 && obj->preferred_node != -1) nid = obj->preferred_node;
  if (nid == -1 && vma && vma->vm_mm) nid = vma->vm_mm->preferred_node;
  if (nid == -1) nid = this_node();

  /*
   * Opportunistic THP:
   * 1. Must be aligned to 2MB (512 pages).
   * 2. Must not have VM_NOHUGEPAGE set.
   * 3. Must fit in object bounds.
   */
  bool thp_eligible = (vmf->pgoff % 512 == 0) &&
                      (vmf->pgoff + 512 <= (obj->size >> PAGE_SHIFT)) &&
                      vma && !(vma->vm_flags & VM_NOHUGEPAGE);

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
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
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
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
  up_write(&obj->lock);

  /* Handle Reverse Mapping (RMAP) - OUTSIDE object lock to avoid deadlock with LRU lock */
  if (vma && vma->anon_vma) {
    folio_add_anon_rmap(folio, vma, vmf->address);
  }

  return 0;
}

static int device_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  uint64_t phys = obj->phys_addr + (vmf->pgoff << PAGE_SHIFT);
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ | VM_WRITE);
  vmm_map_page(vma ? vma->vm_mm : &init_mm, vmf->address, phys, vmf->prot);
  return VM_FAULT_COMPLETED;
}

static const struct vm_object_operations anon_obj_ops = {
  .fault = anon_obj_fault,
};

static const struct vm_object_operations device_obj_ops = {
  .fault = device_obj_fault,
};

static void vm_object_readahead(struct vm_object *obj, struct vm_area_struct *vma, uint64_t pgoff) {
  uint64_t start = pgoff;
  unsigned int size = obj->readahead.size;

  if (size == 0) size = 4; /* Default initial readahead */

  /* Sequential detection */
  if (pgoff == obj->readahead.start + 1) {
    size = size * 2;
    if (size > obj->readahead.ra_pages) size = obj->readahead.ra_pages;
  } else {
    size = 4;
  }

  obj->readahead.start = pgoff;
  obj->readahead.size = size;

  /* Trigger async readahead for 'size' pages */
  for (uint32_t i = 1; i <= size; i++) {
    uint64_t ra_pgoff = pgoff + i;
    if (ra_pgoff >= (obj->size >> PAGE_SHIFT)) break;

    /* Check if already in cache locklessly first */
    if (xa_load(&obj->page_tree, ra_pgoff)) continue;

    /* For now, just allocate and trigger read.
     * Ideally this would be pushed to a workqueue to be truly async. */
    int nid = vma->preferred_node;
    if (nid == -1) nid = this_node();

    struct folio *ra_folio = alloc_pages_node(nid, GFP_KERNEL | __GFP_NOWARN, 0);
    if (!ra_folio) break;

    if (obj->ops && obj->ops->read_folio) {
      /* We don't want to block too long on readahead */
      if (obj->ops->read_folio(obj, ra_folio) == 0) {
        down_write(&obj->lock);
        if (vm_object_add_folio(obj, ra_pgoff, ra_folio) < 0) {
          up_write(&obj->lock);
          folio_put(ra_folio);
        } else {
          up_write(&obj->lock);
          folio_add_file_rmap(ra_folio, obj, ra_pgoff);
        }
      } else {
        folio_put(ra_folio);
      }
    } else {
      folio_put(ra_folio);
      break;
    }
  }
}

static int vnode_obj_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *folio;
  int ret;

  /* 1. Readahead */
  vm_object_readahead(obj, vma, vmf->pgoff);

  /* 2. Check if the page is already in the cache */
  down_read(&obj->lock);
  folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);

    /* If it's a private mapping, we must drop write permission to trigger COW later */
    if (vma && !(vma->vm_flags & VM_SHARED)) {
      vmf->prot &= ~PTE_RW;
    }

    up_read(&obj->lock);
    return 0;
  }
  up_read(&obj->lock);

  /* 2. Bounds check */
  if (vmf->pgoff >= (obj->size >> PAGE_SHIFT)) return VM_FAULT_SIGSEGV;

  /* 3. Allocate a new folio */
  int nid = vma ? vma->preferred_node : obj->preferred_node;
  if (nid == -1) nid = this_node();

  folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!folio) return VM_FAULT_OOM;

  /* 4. Read from the vnode */
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
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    if (vma && !(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;
    return 0;
  }

  if (vm_object_add_folio(obj, vmf->pgoff, folio) < 0) {
    up_write(&obj->lock);
    folio_put(folio);
    return VM_FAULT_SIGBUS;
  }

  folio_get(folio);
  vmf->folio = folio;
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
  if (vma && !(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;

  up_write(&obj->lock);

  /* Link to RMAP for reclamation */
  folio_add_file_rmap(folio, obj, vmf->pgoff);

  return 0;
}

static const struct vm_object_operations vnode_obj_ops = {
  .fault = vnode_obj_fault,
};

struct vm_object *vm_object_vnode_create(struct inode *vnode, size_t size) {
  struct vm_object *obj = vm_object_alloc(VM_OBJECT_VNODE);
  if (!obj) return nullptr;

  obj->vnode = vnode;
  obj->size = size;
  obj->ops = &vnode_obj_ops;
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
 * vm_object_collapse - Deep flattening of shadow chains.
 *
 * This version is more aggressive: it will attempt to merge even if the
 * backing object has multiple references, provided we can "steal" the
 * unique pages.
 */
void vm_object_collapse(struct vm_object *obj) {
  struct vm_object *backing;
  int limit = 16; /* Maximum steps to avoid long stalls */

  if (!obj || !(obj->flags & VM_OBJECT_SHADOW)) return;

  while (limit-- > 0) {
    backing = obj->backing_object;
    if (!backing) break;

    /*
     * We can collapse if:
     * 1. Backing is anonymous.
     * 2. We are the ONLY child of backing.
     */
    if (backing->type == VM_OBJECT_ANON && atomic_read(&backing->shadow_children) == 1) {
      if (!down_write_trylock(&obj->lock)) break;
      if (!down_write_trylock(&backing->lock)) {
        up_write(&obj->lock);
        break;
      }

      /* Re-verify under locks */
      if (atomic_read(&backing->shadow_children) != 1 || (backing->flags & VM_OBJECT_DEAD)) {
        up_write(&backing->lock);
        up_write(&obj->lock);
        break;
      }

      /* Mark collapsing to prevent others from interfering */
      obj->flags |= VM_OBJECT_COLLAPSING;
      backing->flags |= VM_OBJECT_DEAD;

      unsigned long index;
      struct folio *folio;

      /* Flattening: Move all pages from backing to child */
      xa_for_each(&backing->page_tree, index, folio) {
        if (xa_is_err(folio)) continue;

        /* Calculate position in the child object */
        uint64_t backing_offset = index << PAGE_SHIFT;
        if (backing_offset < obj->shadow_offset) continue;

        uint64_t obj_pgoff = (backing_offset - obj->shadow_offset) >> PAGE_SHIFT;
        if (obj_pgoff >= (obj->size >> PAGE_SHIFT)) continue;

        /*
         * If the child already has a page at this offset, the backing
         * page is obsolete and should be dropped. Otherwise, move it.
         */
        if (!xa_load(&obj->page_tree, obj_pgoff)) {
          xa_erase(&backing->page_tree, index);
          if (!((uintptr_t) folio & 0x3)) {
            folio->mapping = (void *) obj;
            folio->index = obj_pgoff;
            atomic_long_inc(&obj->nr_pages);
            atomic_long_dec(&backing->nr_pages);
          }
          xa_store(&obj->page_tree, obj_pgoff, folio, GFP_ATOMIC);
        }
      }

      /* Link to the next object in the chain */
      struct vm_object *new_backing = backing->backing_object;
      if (new_backing) {
        vm_object_get(new_backing);
        atomic_inc(&new_backing->shadow_children);
      }

      obj->backing_object = new_backing;
      obj->shadow_offset += backing->shadow_offset;
      obj->shadow_depth = new_backing ? new_backing->shadow_depth + 1 : 0;
      obj->flags &= ~VM_OBJECT_COLLAPSING;

      /* We are no longer a child of 'backing' */
      atomic_dec(&backing->shadow_children);

      up_write(&backing->lock);
      up_write(&obj->lock);

      /* backing is no longer needed */
      vm_object_put(backing); 
      continue;
    }

    /*
     * BYPASS OPTIMIZATION:
     * If the current shadow object is COMPLETELY EMPTY, we can skip it
     * and point directly to the backing object.
     */
    if (atomic_long_read(&obj->nr_pages) == 0 && (obj->flags & VM_OBJECT_SHADOW)) {
      if (!down_write_trylock(&obj->lock)) break;

      struct vm_object *old_backing = obj->backing_object;
      if (old_backing) {
        /*
         * To safely bypass 'obj', we must update all VMAs that point to it.
         * This requires iterating obj->i_mmap.
         */
        struct vm_area_struct *vma;
        bool all_updated = true;

        /*
         * We need to be careful about locking. VMAs are usually protected by
         * mmap_lock (read) or vma->vm_lock. Since we are changing vma->vm_obj,
         * we ideally need the mmap_lock of the associated mm_struct.
         */
        list_for_each_entry(vma, &obj->i_mmap, vm_shared) {
          /*
           * For each VMA, we adjust its offset and object pointer.
           * This must be done atomically with respect to faults.
           */
          if (vma->vm_mm && !down_write_trylock(&vma->vm_mm->mmap_lock)) {
            all_updated = false;
            break;
          }

          vma->vm_obj = old_backing;
          vma->vm_pgoff += (obj->shadow_offset >> PAGE_SHIFT);
          vm_object_get(old_backing);

          list_del(&vma->vm_shared);
          list_add(&vma->vm_shared, &old_backing->i_mmap);

          if (vma->vm_mm) up_write(&vma->vm_mm->mmap_lock);
        }

        if (all_updated) {
          /*
           * If all VMAs were moved, 'obj' is no longer needed by them.
           * Note: obj->refcount might still be > 0 if other things hold it.
           */
          obj->backing_object = nullptr;
          atomic_dec(&old_backing->shadow_children);
          vm_object_put(old_backing);
        }
      }

      up_write(&obj->lock);
    }

    break;
  }
}

#ifdef CONFIG_MM_SHADOW_ASYNC_COLLAPSE

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
  struct folio *folio = nullptr;
  struct vm_object *curr = obj;
  uint64_t curr_pgoff = vmf->pgoff;
  int ret;

#ifdef CONFIG_MM_SPF
  /*
   * SPECULATIVE PATH:
   * Try to handle locklessly with RCU. This is highly effective for
   * hot pages in short shadow chains.
   */
  if (vmf->flags & FAULT_FLAG_SPECULATIVE) {
    rcu_read_lock();
    struct folio *spec_folio = vm_object_find_folio(curr, curr_pgoff);
    if (spec_folio && folio_try_get(spec_folio)) {
      /*
       * Found it in the immediate shadow object.
       * For speculative faults, we ONLY handle hits in the top object
       * to keep the path extremely fast and avoid complex chain walking.
       */
      vmf->folio = spec_folio;
      vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
      rcu_read_unlock();
      return 0;
    }
    rcu_read_unlock();
    return VM_FAULT_RETRY;
  }
#endif

  /*
   * SHADOW CHAIN ITERATION:
   * Optimized: If depth is too high, trigger collapse early.
   */
#ifdef CONFIG_MM_SHADOW_COLLAPSE
  if (obj->shadow_depth >= obj->collapse_threshold) {
    vm_object_collapse(obj);
    /* Re-fetch depth after potential collapse */
  }
#endif

  while (curr) {
    down_read(&curr->lock);
    folio = vm_object_find_folio(curr, curr_pgoff);
    if (folio) {
      folio_get(folio);
      up_read(&curr->lock);
      break;
    }

    struct vm_object *next = curr->backing_object;
    if (next) {
      uint64_t next_pgoff = curr_pgoff + (curr->shadow_offset >> PAGE_SHIFT);
      up_read(&curr->lock);
      curr = next;
      curr_pgoff = next_pgoff;
    } else {
      /* Reached bottom: if it's a file or device, call its fault handler */
      if (curr->ops && curr->ops->fault) {
        struct vm_fault backing_vmf = *vmf;
        backing_vmf.pgoff = curr_pgoff;

        /* Never allow COW at the bottom of the chain during iteration */
        backing_vmf.flags &= ~FAULT_FLAG_WRITE;

        /* Drop current lock before calling nested fault */
        up_read(&curr->lock);
        ret = curr->ops->fault(curr, vma, &backing_vmf);
        if (ret != 0) return ret;
        folio = backing_vmf.folio;
      } else {
        up_read(&curr->lock);
      }
      break;
    }
  }

  if (!folio) return VM_FAULT_SIGSEGV;

  if ((vmf->flags & FAULT_FLAG_WRITE)) {
    /*
     * Perform Copy-on-Write (COW) if the page was found anywhere
     * except our immediate shadow object.
     */
    if (curr != obj) {
      /*
       * Unique Page Stealing.
       * If the backing object 'curr' is uniquely owned by 'obj', and it's
       * an anonymous object, we can steal the folio instead of copying it.
       */
      bool can_steal = (curr == obj->backing_object) &&
                       (curr->type == VM_OBJECT_ANON) &&
                       (atomic_read(&curr->shadow_children) == 1) &&
                       (atomic_read(&curr->refcount) == 1);

      if (can_steal) {
        down_write(&curr->lock);
        down_write(&obj->lock);
        
        /* Re-verify under locks */
        if (atomic_read(&curr->shadow_children) == 1 && atomic_read(&curr->refcount) == 1) {
          struct folio *stolen = xa_erase(&curr->page_tree, curr_pgoff);
          if (stolen) {
            if (vm_object_add_folio(obj, vmf->pgoff, stolen) == 0) {
              stolen->mapping = (void *) obj;
              stolen->index = vmf->pgoff;
              atomic_long_inc(&obj->nr_pages);
              atomic_long_dec(&curr->nr_pages);
              
              vmf->folio = stolen;
              vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ | VM_WRITE);
              
              up_write(&obj->lock);
              up_write(&curr->lock);
              folio_put(folio); /* Drop the reference from find_folio */
              return 0;
            }
            /* Fallback: put it back if add failed (shouldn't happen with GFP_KERNEL) */
            xa_store(&curr->page_tree, curr_pgoff, stolen, GFP_KERNEL);
          }
        }
        
        up_write(&obj->lock);
        up_write(&curr->lock);
      }

      int nid = vma ? vma->preferred_node : obj->preferred_node;
      if (nid == -1) nid = this_node();

      struct resdomain *rd = obj->rd ? obj->rd : (vma && vma->vm_mm ? vma->vm_mm->rd : nullptr);
      if (rd && resdomain_charge_mem(rd, PAGE_SIZE, false) < 0) {
        folio_put(folio);
        return VM_FAULT_OOM;
      }

      struct folio *new_folio = alloc_pages_node(nid, GFP_KERNEL, 0);
      if (!new_folio) {
        if (rd) resdomain_uncharge_mem(rd, PAGE_SIZE);
        folio_put(folio);
        return VM_FAULT_OOM;
      }
      new_folio->page.rd = rd;

      memcpy(pmm_phys_to_virt(folio_to_phys(new_folio)),
             pmm_phys_to_virt(folio_to_phys(folio)), PAGE_SIZE);

      folio_put(folio);

      down_write(&obj->lock);
      struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);
      if (existing) {
        up_write(&obj->lock);
        if (rd) resdomain_uncharge_mem(rd, PAGE_SIZE);
        folio_put(new_folio);
        folio_get(existing);
        vmf->folio = existing;
        vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ | VM_WRITE);
        return 0;
      }

      vm_object_add_folio(obj, vmf->pgoff, new_folio);
      atomic_long_inc(&obj->nr_pages);
      vmf->folio = new_folio;
      folio_get(new_folio);
      vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ | VM_WRITE);
      up_write(&obj->lock);

      if (vma && vma->anon_vma) folio_add_anon_rmap(new_folio, vma, vmf->address);
      return 0;
    } else {
      /* Page is already in the shadow object, just ensure it's writable */
      vmf->folio = folio;
      vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ | VM_WRITE);
      return 0;
    }
  }

  vmf->folio = folio;
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
  if (curr != obj) vmf->prot &= ~PTE_RW;

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
  atomic_long_inc(&nr_shadow_objects);
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

  /*
   * Preemptive Collapse.
   * If old_obj is a uniquely owned shadow, collapse it now to prevent
   * chain depth explosion during nested forks.
   */
  if ((old_obj->flags & VM_OBJECT_SHADOW) && atomic_read(&old_obj->refcount) == 1) {
    vm_object_collapse(old_obj);
  }

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

void vm_obj_stress_test(void) {
  printk(VMM_CLASS "vm_object: Starting shadow chain stress test...\n");

  /* 1. Create a base object */
  struct vm_object *base = vm_object_anon_create(PAGE_SIZE * 4);
  if (!base) {
    printk(VMM_CLASS "vm_object: Failed to create base object\n");
    return;
  }

  /* 2. Write some known data to it */
  struct folio *folio = alloc_pages(GFP_KERNEL, 0);
  memset(folio_address(folio), 0xAA, PAGE_SIZE);
  vm_object_add_folio(base, 0, folio);
  atomic_long_inc(&base->nr_pages);

  /* 3. Perform nested shadowing */
  struct vm_object *curr = base;
  vm_object_get(base);

  int levels = 100;
  for (int i = 0; i < levels; i++) {
    struct vm_object *shadow = vm_object_shadow_create(curr, 0, PAGE_SIZE * 4);
    if (!shadow) {
      printk(VMM_CLASS "vm_object: Failed at shadow level %d\n", i);
      break;
    }
    
    /* Occasionally write to trigger COW */
    if (i % 10 == 0) {
      struct vm_fault vmf = {
        .pgoff = 0,
        .flags = FAULT_FLAG_WRITE,
        .address = 0x1000, /* Dummy address */
      };
      /* shadow_obj_fault expects a VMA, but we might pass nullptr if we are careful */
      /* For testing, let's just trigger the fault manually if we can */
      int ret = shadow->ops->fault(shadow, nullptr, &vmf);
      if (ret != 0) {
        printk(VMM_CLASS "vm_object: Fault failed at level %d\n", i);
      } else {
        /* Verify data */
        uint8_t *data = pmm_phys_to_virt(folio_to_phys(vmf.folio));
        if (data[0] != 0xAA) {
          printk(VMM_CLASS "vm_object: Data corruption at level %d!\n", i);
        }
        folio_put(vmf.folio);
      }
    }

    vm_object_put(curr);
    curr = shadow;

    /* Periodically try to collapse */
    if (i % 5 == 0) {
      vm_object_collapse(curr);
    }
  }

  int final_depth = vm_object_shadow_depth(curr);
  printk(VMM_CLASS "vm_object: Stress test complete. Final depth: %d (Levels attempted: %d)\n", 
         final_depth, levels);
  
  if (final_depth > 16) {
    printk(VMM_CLASS "vm_object: WARNING: Chain depth too high, collapse might be failing!\n");
  }

  vm_object_put(curr);
  printk(VMM_CLASS "vm_object: All test objects released.\n");
}