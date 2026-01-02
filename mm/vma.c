/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/vma.c
 * @brief Virtual Memory Area (VMA) management
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

#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <crypto/rng.h>
#include <kernel/classes.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
#include <mm/mmu_gather.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vm_object.h>

/* Forward declarations for RMAP (defined in memory.c) */
struct folio;
void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address);

/* ========================================================================
 * RB-Tree Augmentation (Gap Tracking)
 * ======================================================================== */

static inline uint64_t vma_compute_gap(struct vm_area_struct *vma) {
  struct vm_area_struct *prev = vma_prev(vma);
  if (!prev) {
    // Gap between start of address space and this VMA
    return vma->vm_start;
  }
  return vma->vm_start - prev->vm_end;
}

static uint64_t vma_rb_compute_max_gap(struct vm_area_struct *vma) {
  uint64_t max_gap = vma_compute_gap(vma);
  uint64_t cur;

  if (vma->vm_rb.rb_left) {
    cur = rb_entry(vma->vm_rb.rb_left, struct vm_area_struct, vm_rb)->vm_rb_max_gap;
    if (cur > max_gap) max_gap = cur;
  }
  if (vma->vm_rb.rb_right) {
    cur = rb_entry(vma->vm_rb.rb_right, struct vm_area_struct, vm_rb)->vm_rb_max_gap;
    if (cur > max_gap) max_gap = cur;
  }
  return max_gap;
}

RB_DECLARE_CALLBACKS_MAX(static, vma_gap_callbacks,
                         struct vm_area_struct, vm_rb,
                         uint64_t, vm_rb_max_gap, vma_rb_compute_max_gap)

/* ========================================================================
 * VMA Cache Helpers
 * ======================================================================== */

static void vma_cache_update(struct mm_struct *mm, struct vm_area_struct *vma) {
  if (!mm || !vma) return;

  struct task_struct *curr = current;
  // Only cache if we are running in the context of the requested mm
  if (!curr || curr->mm != mm) return;

  // check if already at head
  if (curr->vmacache[0] == vma) return;

  // check if present elsewhere
  for (int i = 1; i < MM_VMA_CACHE_SIZE; i++) {
    if (curr->vmacache[i] == vma) {
      // move to head, shift others down
      for (int j = i; j > 0; j--) {
        curr->vmacache[j] = curr->vmacache[j - 1];
      }
      curr->vmacache[0] = vma;
      return;
    }
  }

  // not found, insert at head, shift everything down
  for (int i = MM_VMA_CACHE_SIZE - 1; i > 0; i--) {
    curr->vmacache[i] = curr->vmacache[i - 1];
  }
  curr->vmacache[0] = vma;
}


/* ========================================================================
 * VMA Cache - Fast allocation using SLAB
 * ======================================================================== */

struct vm_area_struct *vma_cache_alloc(void) {
  return kmalloc(sizeof(struct vm_area_struct));
}

void vma_cache_free(struct vm_area_struct *vma) { kfree(vma); }

/* ========================================================================
 * Bootstrap Allocator - Used before SLAB is ready
 * ======================================================================== */

#define BOOTSTRAP_VMA_COUNT 256
#define BOOTSTRAP_MM_COUNT 16

static struct vm_area_struct bootstrap_vmas[BOOTSTRAP_VMA_COUNT];
static bool bootstrap_vma_in_use[BOOTSTRAP_VMA_COUNT];
static spinlock_t bootstrap_vma_lock = 0;

static struct mm_struct bootstrap_mms[BOOTSTRAP_MM_COUNT];
static bool bootstrap_mm_in_use[BOOTSTRAP_MM_COUNT];
static spinlock_t bootstrap_mm_lock = 0;

static inline bool is_bootstrap_vma(struct vm_area_struct *vma) {
  return (vma >= bootstrap_vmas && vma < bootstrap_vmas + BOOTSTRAP_VMA_COUNT);
}

static inline bool is_bootstrap_mm(struct mm_struct *mm) {
  return (mm >= bootstrap_mms && mm < bootstrap_mms + BOOTSTRAP_MM_COUNT);
}

/* ========================================================================
 * MM Struct Management
 * ======================================================================== */

void mm_init(struct mm_struct *mm) {
  if (!mm)
    return;

  if (mm->map_count > 0 || mm->mm_rb.rb_node != NULL) {
    return;
  }

  memset(mm, 0, sizeof(struct mm_struct));
  mm->mm_rb = RB_ROOT;
  INIT_LIST_HEAD(&mm->mmap_list);
  mm->map_count = 0;
  mm->mmap_base = 0;
  mm->pml4 = NULL;
  spinlock_init(&mm->page_table_lock);
  rwsem_init(&mm->mmap_lock);
  atomic_set(&mm->mm_count, 1);
  mm->vmacache_seqnum = 0;
  cpumask_clear(&mm->cpu_mask);

  /* Initialize memory layout fields */
  mm->start_code = 0;
  mm->end_code = 0;
  mm->start_data = 0;
  mm->end_data = 0;
  mm->start_brk = 0;
  mm->brk = 0;
  mm->start_stack = 0;
}

struct mm_struct *mm_alloc(void) {
  struct mm_struct *mm = NULL;

  mm = kmalloc(sizeof(struct mm_struct));

  if (!mm) {
    spinlock_lock(&bootstrap_mm_lock);
    for (int i = 0; i < BOOTSTRAP_MM_COUNT; i++) {
      if (!bootstrap_mm_in_use[i]) {
        bootstrap_mm_in_use[i] = true;
        mm = &bootstrap_mms[i];
        break;
      }
    }
    spinlock_unlock(&bootstrap_mm_lock);
  }

  if (mm) {
    memset(mm, 0, sizeof(struct mm_struct));
    mm_init(mm);
  }

  return mm;
}

void mm_free(struct mm_struct *mm) {
  if (!mm)
    return;

  mm_destroy(mm);

  if (is_bootstrap_mm(mm)) {
    uint64_t index = mm - bootstrap_mms;
    spinlock_lock(&bootstrap_mm_lock);
    bootstrap_mm_in_use[index] = false;
    spinlock_unlock(&bootstrap_mm_lock);
    return;
  }

  kfree(mm);
}

struct mm_struct *mm_create(void) {
  struct mm_struct *mm = mm_alloc();
  if (!mm)
    return NULL;

  uint64_t pml4_phys = pmm_alloc_page();
  if (!pml4_phys) {
    mm_free(mm);
    return NULL;
  }

  // Initialize PTL for the new PML4 root
  struct page *pg = phys_to_page(pml4_phys);
  spinlock_init(&pg->ptl);

  uint64_t *pml4_virt = (uint64_t *) pmm_phys_to_virt(pml4_phys);
  memset(pml4_virt, 0, PAGE_SIZE);

  // Copy kernel space (higher half, entries 256-511)
  uint64_t *kernel_pml4_virt = (uint64_t *) pmm_phys_to_virt(g_kernel_pml4);

  // Copy higher half (entries 256-511). In x86_64, the root table (PML4 or PML5)
  // always splits the address space in half at index 256.
  memcpy(pml4_virt + 256, kernel_pml4_virt + 256, 256 * sizeof(uint64_t));

  mm->pml4 = (uint64_t *) pml4_phys;
  return mm;
}

void mm_get(struct mm_struct *mm) {
  if (mm) atomic_inc(&mm->mm_count);
}

void mm_put(struct mm_struct *mm) {
  if (!mm) return;
  if (atomic_dec_and_test(&mm->mm_count)) {
    mm_free(mm);
  }
}

struct mm_struct *mm_copy(struct mm_struct *old_mm) {
  struct mm_struct *new_mm = mm_create();
  if (!new_mm)
    return NULL;

  if (!old_mm)
    return new_mm;

  down_write(&old_mm->mmap_lock);

  struct vm_area_struct *vma;
  for_each_vma(old_mm, vma) {
    struct vm_area_struct *new_vma =
        vma_create(vma->vm_start, vma->vm_end, vma->vm_flags);
    if (!new_vma) {
      mm_free(new_mm);
      up_write(&old_mm->mmap_lock);
      return NULL;
    }

    /* Copy metadata */
    new_vma->vm_ops = vma->vm_ops;
    new_vma->vm_private_data = vma->vm_private_data;
    new_vma->vm_pgoff = vma->vm_pgoff;
    
    /* 
     * vm_object handling:
     * vma_create already created a new anon object if RAM.
     * BUT we want to share the object (COW/Shared).
     */
    if (new_vma->vm_obj) vm_object_put(new_vma->vm_obj);
    new_vma->vm_obj = vma->vm_obj;
    if (new_vma->vm_obj) {
        vm_object_get(new_vma->vm_obj);
    }

    if (vma_insert(new_mm, new_vma) != 0) {
      vma_free(new_vma);
      mm_free(new_mm);
      up_write(&old_mm->mmap_lock);
      return NULL;
    }

    /* Notify owner that a new reference was created */
    if (new_vma->vm_ops && new_vma->vm_ops->open) {
      new_vma->vm_ops->open(new_vma);
    }
  }

  /* Copy page tables (COW) */
  if (vmm_copy_page_tables((uint64_t) old_mm->pml4, (uint64_t) new_mm->pml4) < 0) {
    mm_free(new_mm);
    up_write(&old_mm->mmap_lock);
    return NULL;
  }

  up_write(&old_mm->mmap_lock);
  return new_mm;
}

void mm_destroy(struct mm_struct *mm) {
  if (!mm || mm == &init_mm)
    return;

  down_write(&mm->mmap_lock);

  struct vm_area_struct *vma, *tmp;
  for_each_vma_safe(mm, vma, tmp) {
    if (vma->vm_ops && vma->vm_ops->close) {
        vma->vm_ops->close(vma);
    }
    vma_remove(mm, vma);
    vma_free(vma);
  }

  // Free the page tables if it's not the kernel's
  if (mm->pml4 && (uint64_t) mm->pml4 != g_kernel_pml4) {
    vmm_free_page_tables((uint64_t) mm->pml4);
    mm->pml4 = NULL;
  }

  up_write(&mm->mmap_lock);
}

/* ========================================================================
 * VMA Allocation
 * ======================================================================== */

struct vm_area_struct *vma_alloc(void) {
  struct vm_area_struct *vma = NULL;

  vma = vma_cache_alloc();

  if (!vma) {
    spinlock_lock(&bootstrap_vma_lock);
    for (int i = 0; i < BOOTSTRAP_VMA_COUNT; i++) {
      if (!bootstrap_vma_in_use[i]) {
        bootstrap_vma_in_use[i] = true;
        vma = &bootstrap_vmas[i];
        break;
      }
    }
    spinlock_unlock(&bootstrap_vma_lock);
  }

  if (vma) {
    memset(vma, 0, sizeof(struct vm_area_struct));
  }

  return vma;
}

void vma_free(struct vm_area_struct *vma) {
  if (!vma)
    return;

  if (vma->vm_obj) {
    vm_object_put(vma->vm_obj);
  }

  /* Cleanup Chained RMAP entries */
  struct anon_vma_chain *avc, *tmp_avc;
  list_for_each_entry_safe(avc, tmp_avc, &vma->anon_vma_chain, same_vma) {
      struct anon_vma *av = avc->anon_vma;
      
      irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
      list_del(&avc->same_anon_vma);
      spinlock_unlock_irqrestore(&av->lock, flags);
      
      anon_vma_free(av);
      list_del(&avc->same_vma);
      kfree(avc);
  }

  if (is_bootstrap_vma(vma)) {
    uint64_t index = vma - bootstrap_vmas;
    spinlock_lock(&bootstrap_vma_lock);
    bootstrap_vma_in_use[index] = false;
    spinlock_unlock(&bootstrap_vma_lock);
    return;
  }

  vma_cache_free(vma);
}

struct vm_area_struct *vma_create(uint64_t start, uint64_t end,
                                  uint64_t flags) {
  if (start >= end || (start & (PAGE_SIZE - 1)) || (end & (PAGE_SIZE - 1)))
    return NULL;

  struct vm_area_struct *vma = vma_alloc();
  if (!vma)
    return NULL;

  vma->vm_start = start;
  vma->vm_end = end;
  vma->vm_flags = flags;
  vma->vm_mm = NULL;
  
  vma->vm_ops = NULL;
  vma->vm_private_data = NULL;
  vma->vm_pgoff = 0;
  vma->vm_obj = NULL;

  /* 
   * Unified Buffer Cache / vm_object integration:
   * By default, every VMA mapping RAM should have an anonymous vm_object.
   * IO mappings will get their device objects via viomap.
   */
  if (!(flags & (VM_IO | VM_PFNMAP))) {
      vma->vm_obj = vm_object_anon_create(end - start);
  }

  vma->anon_vma = NULL;
  INIT_LIST_HEAD(&vma->anon_vma_chain);
  INIT_LIST_HEAD(&vma->vm_shared);
  RB_CLEAR_NODE(&vma->vm_rb);
  INIT_LIST_HEAD(&vma->vm_list);

  return vma;
}

/* ========================================================================
 * VMA Tree and List Management
 * ======================================================================== */

static void __vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma) {
  struct vm_area_struct *curr;
  struct list_head *pos;

  if (list_empty(&mm->mmap_list)) {
    list_add(&vma->vm_list, &mm->mmap_list);
    return;
  }

  // UBSAN protection: Check for corrupted list head
  if (!mm->mmap_list.next) {
    INIT_LIST_HEAD(&mm->mmap_list);
    list_add(&vma->vm_list, &mm->mmap_list);
    return;
  }

  list_for_each(pos, &mm->mmap_list) {
    curr = list_entry(pos, struct vm_area_struct, vm_list);
    if (curr->vm_start > vma->vm_start) {
      list_add_tail(&vma->vm_list, &curr->vm_list);
      return;
    }
  }

  list_add_tail(&vma->vm_list, &mm->mmap_list);
}

static void __vma_unlink_list(struct vm_area_struct *vma) {
  list_del(&vma->vm_list);
}

static inline bool vma_can_merge(struct vm_area_struct *prev,
                                 struct vm_area_struct *next) {
  if (!prev || !next)
    return false;
  if (prev->vm_end != next->vm_start)
    return false;
  if (prev->vm_flags != next->vm_flags)
    return false;
  if (prev->vm_ops != next->vm_ops)
      return false;
  
  /* Verify VM Objects are compatible (same object, contiguous offsets) */
  if (prev->vm_obj != next->vm_obj)
      return false;
  
  if (prev->vm_obj) {
      if (prev->vm_pgoff + vma_pages(prev) != next->vm_pgoff)
          return false;
  }

  return true;
}

/* ========================================================================
 * VMA Lookup Operations
 * ======================================================================== */

struct vm_area_struct *vma_find(struct mm_struct *mm, uint64_t addr) {
  struct rb_node *node;
  struct vm_area_struct *vma;
  struct task_struct *curr = current;

  if (!mm)
    return NULL;

  /* Check cache first (O(1) optimization) */
  if (curr && curr->mm == mm) {
    if (curr->vmacache_seqnum != mm->vmacache_seqnum) {
      /* Cache invalid, flush it */
      for (int i = 0; i < MM_VMA_CACHE_SIZE; i++) curr->vmacache[i] = NULL;
      curr->vmacache_seqnum = mm->vmacache_seqnum;
    }

    for (int i = 0; i < MM_VMA_CACHE_SIZE; i++) {
      vma = curr->vmacache[i];
      if (vma && addr >= vma->vm_start && addr < vma->vm_end) {
        // If not at head, update LRU
        if (i > 0) vma_cache_update(mm, vma);
        return vma;
      }
    }
  }

  node = mm->mm_rb.rb_node;
  while (node) {
    vma = rb_entry(node, struct vm_area_struct, vm_rb);

    if (addr < vma->vm_start) {
      node = node->rb_left;
    } else if (addr >= vma->vm_end) {
      node = node->rb_right;
    } else {
      vma_cache_update(mm, vma); /* Update cache */
      return vma;
    }
  }

  return NULL;
}

struct vm_area_struct *vma_find_exact(struct mm_struct *mm, uint64_t start,
                                      uint64_t end) {
  struct vm_area_struct *vma = vma_find(mm, start);

  if (vma && vma->vm_start == start && vma->vm_end == end)
    return vma;

  return NULL;
}

struct vm_area_struct *vma_find_intersection(struct mm_struct *mm,
                                             uint64_t start, uint64_t end) {
  if (!mm)
    return NULL;
  struct rb_node *node = mm->mm_rb.rb_node;

  while (node) {
    struct vm_area_struct *vma = rb_entry(node, struct vm_area_struct, vm_rb);

    if (end <= vma->vm_start) {
      node = node->rb_left;
    } else if (start >= vma->vm_end) {
      node = node->rb_right;
    } else {
      return vma; /* Overlap found */
    }
  }

  return NULL;
}

/* ========================================================================
 * VMA Insertion and Removal
 * ======================================================================== */

int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma) {
  struct rb_node **new, *parent = NULL;
  struct vm_area_struct *tmp;

  if (!mm || !vma)
    return -1;
  if (vma->vm_start >= vma->vm_end)
    return -1;

  /* Check for overlap */
  if (vma_find_intersection(mm, vma->vm_start, vma->vm_end)) {
    return -1;
  }

  /* Insert into RB-tree */
  new = &mm->mm_rb.rb_node;
  while (*new) {
    tmp = rb_entry(*new, struct vm_area_struct, vm_rb);
    parent = *new;

    if (vma->vm_start < tmp->vm_start)
      new = &((*new)->rb_left);
    else if (vma->vm_start >= tmp->vm_end)
      new = &((*new)->rb_right);
    else
      return -1;
  }

  vma->vm_mm = mm;

  /* Insert into sorted list */
  __vma_link_list(mm, vma);

  /* Insert into RB-tree */
  rb_link_node(&vma->vm_rb, parent, new);
  rb_insert_augmented(&vma->vm_rb, &mm->mm_rb, &vma_gap_callbacks);

  mm->map_count++;
  mm->vmacache_seqnum++;
  vma_cache_update(mm, vma);

  /* Link to VM Object if present */
  if (vma->vm_obj) {
    spinlock_lock(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    spinlock_unlock(&vma->vm_obj->lock);
  }

  return 0;
}

void vma_remove(struct mm_struct *mm, struct vm_area_struct *vma) {
  if (!mm || !vma)
    return;

  /* Remove from RB-tree */
  rb_erase_augmented(&vma->vm_rb, &mm->mm_rb, &vma_gap_callbacks);
  RB_CLEAR_NODE(&vma->vm_rb);

  /* Remove from list */
  __vma_unlink_list(vma);

  /* Unlink from VM Object if present */
  if (vma->vm_obj) {
    spinlock_lock(&vma->vm_obj->lock);
    list_del(&vma->vm_shared);
    spinlock_unlock(&vma->vm_obj->lock);
  }

  mm->map_count--;
  mm->vmacache_seqnum++;

  vma->vm_mm = NULL;
}

/* ========================================================================
 * VMA Splitting and Merging
 * ======================================================================== */

int vma_split(struct mm_struct *mm, struct vm_area_struct *vma, uint64_t addr) {
  struct vm_area_struct *new_vma;

  if (!mm || !vma)
    return -1;
  if (addr <= vma->vm_start || addr >= vma->vm_end)
    return -1;
  if (addr & (PAGE_SIZE - 1))
    return -1;

  new_vma = vma_create(addr, vma->vm_end, vma->vm_flags);
  if (!new_vma)
    return -1;

  /* 
   * Synchronization: 
   * new_vma was created with a new anonymous object (by vma_create default).
   * We MUST replace it with the original VMA's object to ensure they share pages.
   */
  if (new_vma->vm_obj) vm_object_put(new_vma->vm_obj);
  
  new_vma->vm_obj = vma->vm_obj;
  if (new_vma->vm_obj) vm_object_get(new_vma->vm_obj);
  
  new_vma->vm_pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);
  new_vma->vm_ops = vma->vm_ops;
  new_vma->vm_private_data = vma->vm_private_data;

  /*
   * To safely split, we remove the existing VMA from the tree,
   * modify its end, re-insert it, and then insert the new part.
   * This ensures augmented metadata (max_gap) is correctly updated.
   */
  vma_remove(mm, vma);
  uint64_t old_end = vma->vm_end;
  vma->vm_end = addr;

  if (vma_insert(mm, vma) != 0) {
    // Critical failure (should not happen if addr is valid)
    vma->vm_end = old_end;
    vma_insert(mm, vma); // Attempt recovery
    vma_free(new_vma);
    return -1;
  }

  if (vma_insert(mm, new_vma) != 0) {
    /* Restore original VMA */
    vma_remove(mm, vma);
    vma->vm_end = old_end;
    vma_insert(mm, vma);
    vma_free(new_vma);
    return -1;
  }

  return 0;
}

int vma_merge(struct mm_struct *mm, struct vm_area_struct *vma) {
  struct vm_area_struct *prev, *next;
  int merged = 0;

  if (!mm || !vma)
    return -1;

  /* Try to merge with previous */
  if (!list_is_first(&vma->vm_list, &mm->mmap_list)) {
    prev = list_entry(vma->vm_list.prev, struct vm_area_struct, vm_list);
    if (vma_can_merge(prev, vma)) {
      uint64_t new_end = vma->vm_end;
      vma_remove(mm, vma);
      vma_free(vma);

      vma_remove(mm, prev);
      prev->vm_end = new_end;
      vma_insert(mm, prev);

      vma = prev;
      merged |= VMA_MERGE_PREV;
    }
  }

  /* Try to merge with next */
  if (!list_is_last(&vma->vm_list, &mm->mmap_list)) {
    next = list_entry(vma->vm_list.next, struct vm_area_struct, vm_list);
    if (vma_can_merge(vma, next)) {
      uint64_t new_end = next->vm_end;
      vma_remove(mm, next);
      vma_free(next);

      vma_remove(mm, vma);
      vma->vm_end = new_end;
      vma_insert(mm, vma);

      merged |= VMA_MERGE_NEXT;
    }
  }

  return merged;
}

int vma_expand(struct mm_struct *mm, struct vm_area_struct *vma,
               uint64_t new_start, uint64_t new_end) {
  if (!mm || !vma)
    return -1;
  if (new_start > vma->vm_start || new_end < vma->vm_end)
    return -1;
  if (new_start >= new_end)
    return -1;

  /* Check for conflicts */
  if (new_start < vma->vm_start) {
    if (vma_find_intersection(mm, new_start, vma->vm_start))
      return -1;
  }
  if (new_end > vma->vm_end) {
    if (vma_find_intersection(mm, vma->vm_end, new_end))
      return -1;
  }

  vma_remove(mm, vma);
  vma->vm_start = new_start;
  vma->vm_end = new_end;
  return vma_insert(mm, vma);
}

int vma_shrink(struct mm_struct *mm, struct vm_area_struct *vma,
               uint64_t new_start, uint64_t new_end) {
  if (!mm || !vma)
    return -1;
  if (new_start < vma->vm_start || new_end > vma->vm_end)
    return -1;
  if (new_start >= new_end)
    return -1;

  vma_remove(mm, vma);
  vma->vm_start = new_start;
  vma->vm_end = new_end;
  return vma_insert(mm, vma);
}

/* ========================================================================
 * VMA Iteration
 * ======================================================================== */

struct vm_area_struct *vma_next(struct vm_area_struct *vma) {
  if (!vma || !vma->vm_mm)
    return NULL;
  if (list_is_last(&vma->vm_list, &vma->vm_mm->mmap_list))
    return NULL;
  return list_entry(vma->vm_list.next, struct vm_area_struct, vm_list);
}

struct vm_area_struct *vma_prev(struct vm_area_struct *vma) {
  if (!vma || !vma->vm_mm)
    return NULL;
  if (list_is_first(&vma->vm_list, &vma->vm_mm->mmap_list))
    return NULL;
  return list_entry(vma->vm_list.prev, struct vm_area_struct, vm_list);
}

/* ========================================================================
 * High-level VMA Operations
 * ======================================================================== */

int vma_map_range(struct mm_struct *mm, uint64_t start, uint64_t end,
                  uint64_t flags) {
  struct vm_area_struct *vma;

  if (!mm)
    return -1;
  if (start >= end)
    return -1;

  /* Align to page boundaries */
  start &= ~(PAGE_SIZE - 1);
  end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  down_write(&mm->mmap_lock);

  vma = vma_create(start, end, flags);
  if (!vma) {
    up_write(&mm->mmap_lock);
    return -1;
  }

  if (vma_insert(mm, vma) != 0) {
    vma_free(vma);
    up_write(&mm->mmap_lock);
    return -1;
  }

  /* Try to merge with adjacent VMAs */
  vma_merge(mm, vma);

  up_write(&mm->mmap_lock);
  return 0;
}

int vma_unmap_range(struct mm_struct *mm, uint64_t start, uint64_t end) {
  struct vm_area_struct *vma, *tmp;
  struct mmu_gather tlb;

  if (!mm || start >= end)
    return -1;

  down_write(&mm->mmap_lock);

  /* 1. Split partial overlaps at the boundaries */
  vma = vma_find(mm, start);
  if (vma && vma->vm_start < start) {
    vma_split(mm, vma, start);
  }
  vma = vma_find(mm, end - 1);
  if (vma && vma->vm_start < end && vma->vm_end > end) {
    vma_split(mm, vma, end);
  }

  /* 2. Collect and remove all VMAs now strictly within [start, end] */
  tlb_gather_mmu(&tlb, mm, start, end);

  for_each_vma_safe(mm, vma, tmp) {
    if (vma->vm_end <= start)
      continue;
    if (vma->vm_start >= end)
      break;

    // Unmap physical pages
    if (mm->pml4) {
      for (uint64_t addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
        uint64_t phys;
        vmm_unmap_pages_and_get_phys((uint64_t) mm->pml4, addr, &phys, 1);
        if (phys) {
          tlb_remove_page(&tlb, phys, addr);
        }
      }
    }

    vma_remove(mm, vma);
    vma_free(vma);
  }

  tlb_finish_mmu(&tlb);
  up_write(&mm->mmap_lock);
  return 0;
}

int vma_protect(struct mm_struct *mm, uint64_t start, uint64_t end,
                uint64_t new_flags) {
  struct vm_area_struct *vma;

  if (!mm || start >= end)
    return -1;

  down_write(&mm->mmap_lock);

  /* 1. Split partial overlaps */
  vma = vma_find(mm, start);
  if (vma && vma->vm_start < start) {
    vma_split(mm, vma, start);
  }
  vma = vma_find(mm, end - 1);
  if (vma && vma->vm_start < end && vma->vm_end > end) {
    vma_split(mm, vma, end);
  }

  /* 2. Update flags */
  for_each_vma(mm, vma) {
    if (vma->vm_end <= start)
      continue;
    if (vma->vm_start >= end)
      break;

    vma->vm_flags = new_flags;

    // Translate VMA flags to PTE flags
    uint64_t pte_flags = PTE_USER;
    if (vma->vm_flags & VM_WRITE) pte_flags |= PTE_RW;
    if (!(vma->vm_flags & VM_EXEC)) pte_flags |= PTE_NX;

    if (mm->pml4) {
      for (uint64_t addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
        vmm_set_flags((uint64_t) mm->pml4, addr, pte_flags);
      }
    }
  }

  up_write(&mm->mmap_lock);
  return 0;
}

int mm_populate_user_range(struct mm_struct *mm, uint64_t start, size_t size, uint64_t flags, const uint8_t *data,
                           size_t data_len) {
  if (!mm || size == 0) return -1;

  uint64_t end = (start + size + PAGE_SIZE - 1) & PAGE_MASK;
  start &= PAGE_MASK;

  /* Ensure VMAs exist */
  vma_map_range(mm, start, end, flags | VM_USER);

  uint64_t pml4_phys = (uint64_t) mm->pml4;
  uint64_t pte_flags = PTE_PRESENT | PTE_USER;
  if (flags & VM_WRITE) pte_flags |= PTE_RW;
  if (!(flags & VM_EXEC)) pte_flags |= PTE_NX;

  for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
    struct vm_area_struct *vma = vma_find(mm, addr);
    if (!vma || !vma->vm_obj) return -EINVAL;

    uint64_t pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
    if (vma->vm_pgoff) pgoff += vma->vm_pgoff;

    spinlock_lock(&vma->vm_obj->lock);
    struct page *page = vm_object_find_page(vma->vm_obj, pgoff);
    uint64_t phys;

    if (!page) {
      phys = pmm_alloc_page();
      if (!phys) {
        spinlock_unlock(&vma->vm_obj->lock);
        return -ENOMEM;
      }
      page = phys_to_page(phys);
      memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);
      vm_object_add_page(vma->vm_obj, pgoff, page);
      
      /* If anonymous, setup RMAP */
      if (vma->vm_obj->type == VM_OBJECT_ANON && vma->anon_vma) {
          folio_add_anon_rmap(page_folio(page), vma, addr);
      }
    } else {
      phys = PFN_TO_PHYS(page_to_pfn(page));
    }
    spinlock_unlock(&vma->vm_obj->lock);

    vmm_map_page(pml4_phys, addr, phys, pte_flags);

    // If we have data to copy into this page
    size_t offset = addr - start;
    if (data && offset < data_len) {
      void *virt = pmm_phys_to_virt(phys);
      size_t to_copy = (data_len - offset) > PAGE_SIZE ? PAGE_SIZE : (data_len - offset);
      memcpy(virt, data + offset, to_copy);
    }
  }

  return 0;
}

/* ========================================================================
 * Free Region Finding
 * ======================================================================== */

uint64_t vma_find_free_region(struct mm_struct *mm, size_t size,
                              uint64_t range_start, uint64_t range_end) {
  return vma_find_free_region_aligned(mm, size, PAGE_SIZE, range_start,
                                      range_end);
}

uint64_t vma_find_free_region_aligned(struct mm_struct *mm, size_t size,
                                      uint64_t alignment, uint64_t range_start,
                                      uint64_t range_end) {
  struct rb_node *node;
  uint64_t user_limit = vmm_get_max_user_address();
  bool aslr_attempted = false;
  uint64_t orig_start = range_start;

  if (!mm || size == 0)
    return 0;

  if (range_end == 0) {
    range_end = user_limit;
  }

  if (range_start >= range_end)
    return 0;

  size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

retry:
  /*
   * ASLR: Randomize start address if we are searching a large enough range
   */
  if (!aslr_attempted && (range_end - range_start) > (size + 0x200000)) {
    if (rdrand_supported()) {
      // Use a more aggressive ASLR strategy:
      // Generate a random address within the entire search space
      uint64_t max_offset = range_end - range_start - size;
      // Use 48-bit random number masked to fit
      uint64_t offset = rdrand64();

      offset %= max_offset;
      offset &= ~(alignment - 1);

      range_start += offset;
      aslr_attempted = true;
    }
  }

  node = mm->mm_rb.rb_node;
  if (!node) {
    uint64_t addr = (range_start + alignment - 1) & ~(alignment - 1);
    // Guard against NULL page
    if (addr == 0) addr = PAGE_SIZE;

    if (addr + size <= range_end) return addr;

    if (aslr_attempted) {
      range_start = orig_start;
      aslr_attempted = false;
      goto retry;
    }
    return 0;
  }

  struct vm_area_struct *best_vma = NULL;
  node = mm->mm_rb.rb_node;

  while (node) {
    struct vm_area_struct *vma = rb_entry(node, struct vm_area_struct, vm_rb);

    if (vma->vm_start <= range_start) {
      node = node->rb_right;
      continue;
    }

    struct vm_area_struct *prev = vma_prev(vma);
    uint64_t prev_end = prev ? prev->vm_end : 0;
    if (prev_end < range_start) prev_end = range_start;

    // Candidate address
    uint64_t candidate = (prev_end + alignment - 1) & ~(alignment - 1);

    // Enforce Guard Page (Gap) from Previous
    if (prev && candidate == prev->vm_end) {
      candidate += PAGE_SIZE;
      candidate = (candidate + alignment - 1) & ~(alignment - 1);
    }
    if (candidate == 0) candidate = PAGE_SIZE;

    // Check fit before current VMA
    // Enforce Guard Page (Gap) to Next: candidate + size < vma->vm_start
    // So candidate + size + PAGE_SIZE <= vma->vm_start
    // (Using PAGE_SIZE as implicit guard)
    if (candidate + size + PAGE_SIZE <= vma->vm_start) {
      best_vma = vma;
      node = node->rb_left;
    } else {
      // Check if left subtree has a large enough gap.
      // The max_gap must be big enough to hold:
      // [guard][size][guard] (worst case)
      // We need roughly size + 2*PAGE_SIZE space in a gap to be sure we can fit
      // a guarded region?
      // Actually, if max_gap >= size + 2*PAGE_SIZE, it's definitely worth looking.
      if (node->rb_left && rb_entry(node->rb_left, struct vm_area_struct, vm_rb)->vm_rb_max_gap >= size + 2 *
          PAGE_SIZE) {
        node = node->rb_left;
      } else {
        node = node->rb_right;
      }
    }
  }

  if (best_vma) {
    struct vm_area_struct *prev = vma_prev(best_vma);
    uint64_t prev_end = prev ? prev->vm_end : 0;
    if (prev_end < range_start) prev_end = range_start;

    uint64_t addr = (prev_end + alignment - 1) & ~(alignment - 1);
    if (prev && addr == prev->vm_end) {
      addr += PAGE_SIZE;
      addr = (addr + alignment - 1) & ~(alignment - 1);
    }
    if (addr == 0) addr = PAGE_SIZE;

    return addr;
  }

  /* Check final gap (after the last VMA) */
  struct rb_node *last_node = mm->mm_rb.rb_node;
  while (last_node && last_node->rb_right) last_node = last_node->rb_right;

  uint64_t addr;
  if (last_node) {
    struct vm_area_struct *last_vma = rb_entry(last_node, struct vm_area_struct, vm_rb);
    addr = (last_vma->vm_end + alignment - 1) & ~(alignment - 1);
    // Guard from last VMA
    if (addr == last_vma->vm_end) {
      addr += PAGE_SIZE;
      addr = (addr + alignment - 1) & ~(alignment - 1);
    }
    if (addr < range_start) addr = (range_start + alignment - 1) & ~(alignment - 1);
  } else {
    addr = (range_start + alignment - 1) & ~(alignment - 1);
    if (addr == 0) addr = PAGE_SIZE;
  }

  if (addr + size <= range_end)
    return addr;

  // Retry without ASLR if we failed
  if (aslr_attempted) {
    range_start = orig_start;
    aslr_attempted = false;
    goto retry;
  }

  return 0;
}

/* ========================================================================
 * Statistics and Debugging
 * ======================================================================== */

size_t mm_total_size(struct mm_struct *mm) {
  struct vm_area_struct *vma;
  size_t total = 0;

  if (!mm)
    return 0;

  for_each_vma(mm, vma) { total += vma_size(vma); }

  return total;
}

size_t mm_map_count(struct mm_struct *mm) { return mm ? mm->map_count : 0; }

void vma_dump_single(struct vm_area_struct *vma) {
  if (!vma)
    return;

  printk(VMA_CLASS "  VMA [%016llx - %016llx] size: %8llu KB, flags: %08llx (",
         vma->vm_start, vma->vm_end, vma_size(vma) / 1024, vma->vm_flags);

  if (vma->vm_flags & VM_READ)
    printk("r");
  if (vma->vm_flags & VM_WRITE)
    printk("w");
  if (vma->vm_flags & VM_EXEC)
    printk("x");
  if (vma->vm_flags & VM_SHARED)
    printk("s");
  if (vma->vm_flags & VM_LOCKED)
    printk("l");
  printk(")\n");
}

void vma_dump(struct mm_struct *mm) {
  struct vm_area_struct *vma;

  if (!mm)
    return;

  printk(VMA_CLASS "[--- VMA Dump for MM: %p (%d-level paging) ---]\n", mm, vmm_get_paging_levels());
  printk(VMA_CLASS "Canonical High Base: %016llx\n", vmm_get_canonical_high_base());
  printk(VMA_CLASS "Total VMAs: %d, Total VM: %llu KB\n", mm->map_count,
         mm_total_size(mm) / 1024);
  printk(VMA_CLASS "Code: [%llx - %llx], Data: [%llx - %llx]\n", mm->start_code,
         mm->end_code, mm->start_data, mm->end_data);
  printk(VMA_CLASS "Brk: [%llx - %llx], Stack: %llx\n", mm->start_brk, mm->brk,
         mm->start_stack);

  for_each_vma(mm, vma) { vma_dump_single(vma); }
}

/* ========================================================================
 * Validation and Testing
 * ======================================================================== */

int vma_verify_tree(struct mm_struct *mm) {
  /* Could implement RB-tree property verification here */
  return 0;
}

int vma_verify_list(struct mm_struct *mm) {
  struct vm_area_struct *vma, *prev = NULL;

  for_each_vma(mm, vma) {
    if (prev && prev->vm_start >= vma->vm_start) {
      printk(VMA_CLASS "ERROR: List not sorted!\n");
      return -1;
    }
    if (prev && prev->vm_end > vma->vm_start) {
      printk(VMA_CLASS "ERROR: Overlapping VMAs in list!\n");
      return -1;
    }
    prev = vma;
  }

  return 0;
}

/* ========================================================================
 * Global Kernel MM
 * ======================================================================== */

struct mm_struct init_mm;

/* ========================================================================
 * Test Suite
 * ======================================================================== */

void vma_test(void) {
  printk(KERN_DEBUG VMA_CLASS "Starting VMA Test Suite...\n");

  /* Use mm_create to get valid page tables and exercise VMM glue */
  struct mm_struct *mm = mm_create();
  if (!mm) panic("vma_test: failed to create mm");

  /* Test 1: Basic Mappings & RB-Tree Insertion */
  // Create two 2-page VMAs: [0x1000, 0x3000] and [0x5000, 0x7000]
  vma_map_range(mm, 0x1000, 0x3000, VM_READ);
  vma_map_range(mm, 0x5000, 0x7000, VM_READ | VM_WRITE);

  if (mm->map_count != 2) panic("vma_test: map_count mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - Basic Mapping: OK\n");

  /* Test 2: Gap Finding (Augmented RB-Tree) */
  // NOTE: With guard pages, we can't fit 4KB in the 8KB gap between 0x3000 and 0x5000.
  // 0x3000 (end of VMA1) -> Guard (0x4000) -> Data (0x5000) -> Collision with VMA2.
  // So it should find space AFTER 0x7000.
  // Expected: 0x7000 + Guard(0x1000) = 0x8000.
  uint64_t free = vma_find_free_region(mm, 0x1000, 0x1000, 0x10000);

  // Depending on ASLR, this might be higher, but we restricted ASLR range start to low.
  // However, vma_find_free_region uses ASLR now.
  // To test deterministic behavior, we should perhaps rely on 'alignment' or just verify it's valid.

  if (free == 0) panic("vma_test: gap find failed completely");

  // Verify it doesn't overlap or touch
  struct vm_area_struct *v1 = vma_find(mm, free);
  if (v1) panic("vma_test: allocated on existing VMA");

  // Check overlap with 0x1000-0x3000
  if (free >= 0x1000 && free < 0x3000) panic("vma_test: overlap VMA1");
  // Check overlap with 0x5000-0x7000
  if (free >= 0x5000 && free < 0x7000) panic("vma_test: overlap VMA2");

  // Check guard pages
  if (free == 0x3000 || free + 0x1000 == 0x5000) panic("vma_test: guard page violation");

  printk(KERN_DEBUG VMA_CLASS "  - Gap Finding: OK (Got %llx)\n", free);

  /* Test 3: VMA Splitting (Must be page aligned) */
  printk(KERN_DEBUG VMA_CLASS "  - VMA Splitting: start...\n");
  down_write(&mm->mmap_lock);
  struct vm_area_struct *vma_to_split = vma_find(mm, 0x5000);
  if (!vma_to_split) panic("vma_test: could not find vma at 0x5000");

  // Split [0x5000, 0x7000] at 0x6000
  if (vma_split(mm, vma_to_split, 0x6000) != 0) {
    panic("vma_test: split failed");
  }
  up_write(&mm->mmap_lock);

  if (mm->map_count != 3) panic("vma_test: map_count after split mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - VMA Splitting: OK\n");

  /* Test 4: VMA Protection (with split) */
  printk(KERN_DEBUG VMA_CLASS "  - VMA Protect (Split): start...\n");
  // Change protection on the first page of [0x1000, 0x3000]
  if (vma_protect(mm, 0x1000, 0x2000, VM_READ | VM_WRITE) != 0) panic("vma_test: protect failed");
  if (mm->map_count != 4) panic("vma_test: map_count after protect mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - VMA Protect (Split): OK\n");

  /* Test 5: Unmapping partial & full */
  // Unmap the middle pages across multiple VMAs: [0x2000, 0x6000]
  vma_unmap_range(mm, 0x2000, 0x6000);
  printk(KERN_DEBUG VMA_CLASS "  - Partial Unmap: OK\n");

  /* Clean up all */
  vma_unmap_range(mm, 0, 0xFFFFFFFFFFFFFFFF);
  if (mm->map_count != 0) panic("vma_test: unmap all failed");
  printk(KERN_DEBUG VMA_CLASS "  - Unmap All: OK\n");

  mm_destroy(mm);
  mm_free(mm);
  printk(KERN_DEBUG VMA_CLASS "VMA Test Suite Passed.\n");
}
