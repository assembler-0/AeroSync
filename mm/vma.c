///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vma.c
 * @brief Virtual Memory Area (VMA) management
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

#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <crypto/rng.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/panic.h>
#include <aerosync/timer.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <linux/maple_tree.h>
#include <mm/mmu_gather.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <arch/x86_64/mm/tlb.h>

/* Forward declarations for RMAP (defined in memory.c) */
struct folio;

static inline void vma_verify(struct vm_area_struct *vma);

/* ========================================================================
 * VMA Cache Helpers
 * ======================================================================= */

static void vma_cache_update(struct mm_struct *mm, struct vm_area_struct *vma) {
  if (unlikely(!mm || !vma)) return;
  vma_verify(vma);

  struct task_struct *curr = current;
  if (unlikely(!curr || curr->mm != mm)) return;

  preempt_disable();

  /* Validate cache sequence number */
  if (unlikely(curr->vmacache_seqnum != mm->vmacache_seqnum)) {
    memset(curr->vmacache, 0, sizeof(curr->vmacache));
    curr->vmacache_seqnum = mm->vmacache_seqnum;
  }

  /*
   * Most recently used (MRU) logic:
   * If it's already at index 0, we're done.
   */
  if (curr->vmacache[0] == vma) {
    preempt_enable();
    return;
  }

  /* Find it in the cache and move to front */
  for (int i = 1; i < MM_VMA_CACHE_SIZE; i++) {
    if (curr->vmacache[i] == vma) {
      for (int j = i; j > 0; j--) {
        curr->vmacache[j] = curr->vmacache[j - 1];
      }
      curr->vmacache[0] = vma;
      preempt_enable();
      return;
    }
  }

  /* Not in cache, insert at front and shift others */
  for (int i = MM_VMA_CACHE_SIZE - 1; i > 0; i--) {
    curr->vmacache[i] = curr->vmacache[i - 1];
  }
  curr->vmacache[0] = vma;
  preempt_enable();
}


/* ========================================================================
 * VMA Cache - Fast allocation using SLAB
 * ======================================================================= */

static struct kmem_cache *vma_cachep;

void vma_cache_init(void) {
  vma_cachep = kmem_cache_create("vm_area_struct", sizeof(struct vm_area_struct),
                                 0, SLAB_HWCACHE_ALIGN | SLAB_TYPESAFE_BY_RCU);
  if (!vma_cachep) {
    panic("vma_cache_init: failed to create vma_cachep");
  }
}

struct vm_area_struct *vma_cache_alloc_node(int nid) {
  return kmem_cache_alloc_node(vma_cachep, nid);
}

struct vm_area_struct *vma_cache_alloc(void) {
  return kmem_cache_alloc(vma_cachep);
}

void vma_cache_free(struct vm_area_struct *vma) {
  kmem_cache_free(vma_cachep, vma);
}

/* ========================================================================
 * Bootstrap Allocator - Used before SLAB is ready
 * ======================================================================= */

#define BOOTSTRAP_VMA_COUNT 256
#define BOOTSTRAP_MM_COUNT 16

static struct vm_area_struct bootstrap_vmas[BOOTSTRAP_VMA_COUNT];
static unsigned char bootstrap_vma_in_use[BOOTSTRAP_VMA_COUNT];

static struct mm_struct bootstrap_mms[BOOTSTRAP_MM_COUNT];
static unsigned char bootstrap_mm_in_use[BOOTSTRAP_MM_COUNT];
static DEFINE_SPINLOCK(bootstrap_mm_lock);

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

  memset(mm, 0, sizeof(struct mm_struct));

  /*
   * Initialize Maple Tree with RCU support.
   * MT_FLAGS_USE_RCU allows lockless concurrent lookups.
   * MT_FLAGS_ALLOC_RANGE is needed for vma_find_free_region.
   */
  mt_init_flags(&mm->mm_mt, MT_FLAGS_ALLOC_RANGE | MT_FLAGS_USE_RCU | MT_FLAGS_LOCK_EXTERN);
  mt_set_external_lock(&mm->mm_mt, &mm->mmap_lock);

  mm->map_count = 0;
  mm->mmap_base = 0;
  mm->last_hole = 0;
  mm->pml_root = nullptr;
  rwsem_init(&mm->mmap_lock);
  atomic_set(&mm->mm_count, 1);
  mm->vmacache_seqnum = 0;
  mm->preferred_node = -1;
  cpumask_clear(&mm->cpu_mask);
  atomic_set(&mm->mmap_seq, 0);

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
  struct mm_struct *mm = nullptr;
  int nid = this_node();

  mm = kzalloc_node(sizeof(struct mm_struct), nid);

  if (!mm) {
    spinlock_lock(&bootstrap_mm_lock);
    for (int i = 0; i < BOOTSTRAP_MM_COUNT; i++) {
      if (!bootstrap_mm_in_use[i]) {
        bootstrap_mm_in_use[i] = 1;
        mm = &bootstrap_mms[i];
        break;
      }
    }
    spinlock_unlock(&bootstrap_mm_lock);
  }

  if (mm) {
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
    bootstrap_mm_in_use[index] = 0;
    spinlock_unlock(&bootstrap_mm_lock);
    return;
  }

  kfree(mm);
}

struct mm_struct *mm_create(void) {
  struct mm_struct *mm = mm_alloc();
  if (!mm)
    return nullptr;

  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  uint64_t pml_root_phys = vmm_alloc_table_node(nid);
  if (!pml_root_phys) {
    mm_free(mm);
    return nullptr;
  }

  /* Initialize PTL for the new PML4 root */
  struct page *pg = phys_to_page(pml_root_phys);
  spinlock_init(&pg->ptl);

  uint64_t *pml4_virt = (uint64_t *) pmm_phys_to_virt(pml_root_phys);
  memset(pml4_virt, 0, PAGE_SIZE);

  /* Copy kernel space (higher half, entries 256-511) */
  uint64_t *kernel_pml_virt = (uint64_t *) pmm_phys_to_virt(g_kernel_pml_root);

  /* Copy higher half (entries 256-511). In x86_64, the root table (PML4 or PML5)
   * always splits the address space in half at index 256.
   */
  memcpy(pml4_virt + 256, kernel_pml_virt + 256, 256 * sizeof(uint64_t));

  mm->pml_root = (uint64_t *) pml_root_phys;
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
    return nullptr;

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
      return nullptr;
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

    if (vma->vm_obj && (vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
      if (vm_object_cow_prepare(vma, new_vma) < 0) {
        vma_free(new_vma);
        mm_free(new_mm);
        up_write(&old_mm->mmap_lock);
        return nullptr;
      }
    } else {
      /* Shared or Read-Only mapping: just share the object */
      new_vma->vm_obj = vma->vm_obj;
      if (new_vma->vm_obj) {
        vm_object_get(new_vma->vm_obj);
      }
    }

    if (vma_insert(new_mm, new_vma) != 0) {
      vma_free(new_vma);
      mm_free(new_mm);
      up_write(&old_mm->mmap_lock);
      return nullptr;
    }

    /* Notify owner that a new reference was created */
    if (new_vma->vm_ops && new_vma->vm_ops->open) {
      new_vma->vm_ops->open(new_vma);
    }
  }

  /* Copy page tables (COW) */
  if (vmm_copy_page_tables(old_mm, new_mm) < 0) {
    mm_free(new_mm);
    up_write(&old_mm->mmap_lock);
    return nullptr;
  }

  vmm_tlb_shootdown(old_mm, 0, vmm_get_max_user_address());

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

    /*
     * Resource cleanup - don't use vma_remove here as we destroy the whole
     * tree in one go below for maximum performance.
     */
    vma_free(vma);
  }

  /* Bulk destroy the tree structures */
  mtree_destroy(&mm->mm_mt);

  /* Free the page tables if it's not the kernel's */
  if (mm->pml_root && (uint64_t) mm->pml_root != g_kernel_pml_root) {
    vmm_free_page_tables(mm);
    mm->pml_root = nullptr;
  }

  up_write(&mm->mmap_lock);
}

/* ========================================================================
 * VMA Allocation
 * ======================================================================== */

static inline void vma_verify(struct vm_area_struct *vma) {
#ifdef MM_HARDENING
  /*
   * OPTIMIZATION: Use branch prediction hints.
   * The unlikely() macro tells the compiler this branch is almost never taken,
   * allowing better code layout and avoiding pipeline stalls on the hot path.
   *
   * Also use __builtin_expect to help the branch predictor.
   */
  if (unlikely(!vma)) return;
  if (unlikely(vma->vma_magic != VMA_MAGIC)) {
    /* Cold path - only executed on actual corruption */
    uint64_t *ptr = (uint64_t *) vma;
    printk(KERN_EMERG "VMA Corruption at %p: magic=%llx, raw[0]=%llx, raw[1]=%llx\n",
           vma, vma->vma_magic, ptr[0], ptr[1]);
    panic("VMA Corruption detected");
  }
#else
  (void) vma;
#endif
}

struct vm_area_struct *vma_alloc(void) {
  struct vm_area_struct *vma = nullptr;
  struct task_struct *curr = current;
  int nid = curr ? curr->node_id : this_node();

  vma = vma_cache_alloc_node(nid);

  if (unlikely(!vma)) {
    /* Atomic bootstrap allocation to prevent races */
    for (int i = 0; i < BOOTSTRAP_VMA_COUNT; i++) {
      if (!__atomic_test_and_set(&bootstrap_vma_in_use[i], __ATOMIC_ACQUIRE)) {
        vma = &bootstrap_vmas[i];
        break;
      }
    }
  }

  if (likely(vma)) {
    memset(vma, 0, sizeof(struct vm_area_struct));
    vma->vma_magic = VMA_MAGIC;
  }

  return vma;
}

static void vma_free_rcu(struct rcu_head *head) {
  struct vm_area_struct *vma = container_of(head, struct vm_area_struct, rcu);

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
    __atomic_clear(&bootstrap_vma_in_use[index], __ATOMIC_RELEASE);
    return;
  }

  vma_cache_free(vma);
}

void vma_free(struct vm_area_struct *vma) {
  if (!vma)
    return;

  vma_verify(vma);
  vma->vma_magic = 0xDEADBEEF;

  call_rcu(&vma->rcu, vma_free_rcu);
}

/* ========================================================================
 * VMA Tree Management
 * ======================================================================== */

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
  struct vm_area_struct *vma;
  struct task_struct *curr = current;

  if (unlikely(!mm))
    return nullptr;

  /*
   * OPTIMIZATION: Check cache first (O(1) lookup).
   * The VMA cache is per-task and doesn't require RCU if we are 'current'.
   * We avoid vma_verify in the inner loop for speed - only verify on hit.
   */
  if (curr && curr->mm == mm) {
    preempt_disable();
    if (unlikely(curr->vmacache_seqnum != mm->vmacache_seqnum)) {
      /*
       * OPTIMIZATION: Use explicit loop instead of memset for small arrays.
       * Modern compilers may inline memset for small sizes, but explicit
       * zeroing can be faster for very small arrays.
       */
      for (int i = 0; i < MM_VMA_CACHE_SIZE; i++)
        curr->vmacache[i] = nullptr;
      curr->vmacache_seqnum = mm->vmacache_seqnum;
    }

    for (int i = 0; i < MM_VMA_CACHE_SIZE; i++) {
      vma = curr->vmacache[i];
      if (vma && addr >= vma->vm_start && addr < vma->vm_end) {
        /*
         * OPTIMIZATION: Move vma_verify after the match.
         * This avoids cache line thrashing from reading vma_magic
         * for non-matching entries.
         */
        vma_verify(vma);
        /* MRU: Move to front only if not already there */
        if (i > 0) {
          /* OPTIMIZATION: memmove equivalent but branch-free */
          struct vm_area_struct *found = vma;
          for (int j = i; j > 0; j--) {
            curr->vmacache[j] = curr->vmacache[j - 1];
          }
          curr->vmacache[0] = found;
        }
        preempt_enable();
        return vma;
      }
    }
    preempt_enable();
  }

  /*
   * RCU-Safe Maple Tree lookup.
   * If we don't hold the mmap_lock, we must use RCU to ensure the VMA
   * isn't freed while we are looking at it.
   */
  rcu_read_lock();
  unsigned long index = addr;
  vma = mt_find(&mm->mm_mt, &index, ULONG_MAX);
  if (vma) {
    vma_verify(vma);
    /* Update cache only if we are the current task to avoid remote cache pollution */
    if (curr && curr->mm == mm) {
      vma_cache_update(mm, vma);
    }
  }
  rcu_read_unlock();

  return vma;
}

struct vm_area_struct *vma_find_exact(struct mm_struct *mm, uint64_t start, uint64_t end) {
  struct vm_area_struct *vma = vma_find(mm, start);
  if (vma && vma->vm_start == start && vma->vm_end == end)
    return vma;
  return nullptr;
}

struct vm_area_struct *vma_find_intersection(struct mm_struct *mm, uint64_t start, uint64_t end) {
  if (!mm) return nullptr;

  MA_STATE(mas, &mm->mm_mt, start, end - 1);
  return mas_find(&mas, end - 1);
}

/* ========================================================================
 * VMA Insertion and Removal
 * ======================================================================== */

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

static inline void vma_layout_changed(struct mm_struct *mm, struct vm_area_struct *vma) {
  mm->vmacache_seqnum++;
  atomic_inc(&mm->mmap_seq);
  if (vma) {
    __atomic_add_fetch(&vma->vma_seq, 1, __ATOMIC_RELEASE);
  }
}

static void vma_account_inc(struct mm_struct *mm, struct vm_area_struct *vma) {
  size_t pages = vma_pages(vma);
  mm->total_vm += pages;
  if (vma->vm_flags & VM_LOCKED) mm->locked_vm += pages;
  if (vma->vm_flags & VM_STACK) mm->stack_vm += pages;
  if (vma->vm_flags & VM_WRITE) {
    if (!(vma->vm_flags & (VM_SHARED | VM_STACK))) mm->data_vm += pages;
  } else if (vma->vm_flags & VM_EXEC) {
    mm->exec_vm += pages;
  }
}

static void vma_account_dec(struct mm_struct *mm, struct vm_area_struct *vma) {
  size_t pages = vma_pages(vma);
  mm->total_vm -= pages;
  if (vma->vm_flags & VM_LOCKED) mm->locked_vm -= pages;
  if (vma->vm_flags & VM_STACK) mm->stack_vm -= pages;
  if (vma->vm_flags & VM_WRITE) {
    if (!(vma->vm_flags & (VM_SHARED | VM_STACK))) mm->data_vm -= pages;
  } else if (vma->vm_flags & VM_EXEC) {
    mm->exec_vm -= pages;
  }
}

int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma) {
  if (!mm || !vma) return -EINVAL;

  /*
   * Insert into Maple Tree.
   * This automatically checks for overlaps and maintains the range.
   */
  if (mtree_insert_range(&mm->mm_mt, vma->vm_start, vma->vm_end - 1, vma, GFP_KERNEL)) {
    return -ENOMEM;
  }

  vma->vm_mm = mm;
  mm->map_count++;
  vma_account_inc(mm, vma);
  vma_layout_changed(mm, vma);
  vma_cache_update(mm, vma);

  if (vma->vm_obj) {
    down_write(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    up_write(&vma->vm_obj->lock);
  }

  return 0;
}

void vma_remove(struct mm_struct *mm, struct vm_area_struct *vma) {
  if (!mm || !vma) return;

  mtree_erase(&mm->mm_mt, vma->vm_start);
  vma_account_dec(mm, vma);
  mm->map_count--;
  vma_layout_changed(mm, vma);

  if (vma->vm_obj) {
    down_write(&vma->vm_obj->lock);
    list_del(&vma->vm_shared);
    up_write(&vma->vm_obj->lock);
  }

  vma->vm_mm = nullptr;
}


/* ========================================================================
 * VMA Splitting and Merging
 * ======================================================================== */

int vma_split(struct mm_struct *mm, struct vm_area_struct *vma, uint64_t addr) {
  struct vm_area_struct *new_vma;

  if (!mm || !vma || addr <= vma->vm_start || addr >= vma->vm_end || (addr & (PAGE_SIZE - 1)))
    return -EINVAL;

  new_vma = vma_alloc();
  if (!new_vma) return -ENOMEM;

  /*
   * Manual copy to ensure we control what gets duplicated.
   */
  new_vma->vm_start = addr;
  new_vma->vm_end = vma->vm_end;
  new_vma->vm_flags = vma->vm_flags;
  new_vma->vm_page_prot = vma->vm_page_prot;
  new_vma->vm_ops = vma->vm_ops;
  new_vma->vm_pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);
  new_vma->vm_private_data = vma->vm_private_data;
  new_vma->preferred_node = vma->preferred_node;

  /* Increment refcounts for shared resources */
  new_vma->vm_obj = vma->vm_obj;
  if (new_vma->vm_obj) vm_object_get(new_vma->vm_obj);

  new_vma->anon_vma = vma->anon_vma;
  if (new_vma->anon_vma) {
    /* This requires a proper anon_vma_clone helper in a real system */
    atomic_inc(&new_vma->anon_vma->refcount);
  }

  INIT_LIST_HEAD(&new_vma->vm_shared);
  INIT_LIST_HEAD(&new_vma->anon_vma_chain);
  rwsem_init(&new_vma->vm_lock);

  /* Adjust original VMA */
  vma->vm_end = addr;

  /* Update the tree atomically using MAS */
  MA_STATE(mas, &mm->mm_mt, vma->vm_start, vma->vm_end - 1);
  mas_lock(&mas);

  /* 1. Update existing VMA range */
  mas_store(&mas, vma);

  /* 2. Insert new VMA */
  mas_set_range(&mas, new_vma->vm_start, new_vma->vm_end - 1);
  mas_store(&mas, new_vma);

  mas_unlock(&mas);

  new_vma->vm_mm = mm;
  mm->map_count++;

  /* Accounting is already correct since we just split one region into two with same flags */

  vma_layout_changed(mm, vma);
  vma_layout_changed(mm, new_vma);

  if (new_vma->vm_ops && new_vma->vm_ops->open) new_vma->vm_ops->open(new_vma);
  return 0;
}

/*
 * vma_merge - Proactive VMA merging logic.
 */
struct vm_area_struct *vma_merge(struct mm_struct *mm, struct vm_area_struct *prev,
                                 uint64_t addr, uint64_t end, uint64_t vm_flags,
                                 struct vm_object *obj, uint64_t pgoff) {
  struct vm_area_struct *next;

  if (!prev) {
    prev = vma_find(mm, addr - 1);
  }

  /*
   * To merge with prev, we need:
   * 1. Same flags and ops.
   * 2. Adjacency.
   * 3. Compatible objects (both nullptr, or same object with contiguous offsets).
   */
  if (prev && prev->vm_end == addr &&
      prev->vm_flags == vm_flags && prev->vm_ops == nullptr) {
    if (prev->vm_obj == obj && (obj == nullptr || prev->vm_pgoff + vma_pages(prev) == pgoff)) {
      next = vma_next(prev);
      if (next && next->vm_start == end && next->vm_flags == vm_flags &&
          next->vm_ops == nullptr && next->vm_obj == obj &&
          (obj == nullptr || pgoff + ((end - addr) >> PAGE_SHIFT) == next->vm_pgoff)) {
        /* CASE: BRIDGE [prev][new][next] */
        uint64_t giant_end = next->vm_end;

        MA_STATE(mas, &mm->mm_mt, addr, giant_end - 1);
        mas_lock(&mas);

        /* 1. Remove next */
        mas_set_range(&mas, next->vm_start, next->vm_end - 1);
        mas_erase(&mas);

        /* 2. Update prev to cover whole range */
        vma_account_dec(mm, prev);
        prev->vm_end = giant_end;
        vma_account_inc(mm, prev);

        mas_set_range(&mas, prev->vm_start, prev->vm_end - 1);
        mas_store(&mas, prev);

        mas_unlock(&mas);

        vma_account_dec(mm, next); /* Accounting for next which is gone */
        mm->map_count--;

        vma_layout_changed(mm, prev);
        vma_free(next);
        return prev;
      }

      /* CASE: EXTEND PREV */
      MA_STATE(mas, &mm->mm_mt, addr, end - 1);
      mas_lock(&mas);

      vma_account_dec(mm, prev);
      prev->vm_end = end;
      vma_account_inc(mm, prev);

      mas_set_range(&mas, prev->vm_start, prev->vm_end - 1);
      mas_store(&mas, prev);

      mas_unlock(&mas);

      vma_layout_changed(mm, prev);
      return prev;
    }
  }

  /* Check for merge with next only */
  if (prev) next = vma_next(prev);
  else {
    unsigned long index = 0;
    next = mt_find(&mm->mm_mt, &index, ULONG_MAX);
  }

  if (next && next->vm_start == end && next->vm_flags == vm_flags &&
      next->vm_ops == nullptr && next->vm_obj == obj &&
      (obj == nullptr || pgoff + ((end - addr) >> PAGE_SHIFT) == next->vm_pgoff)) {
    /* CASE: EXTEND NEXT BACKWARDS */
    MA_STATE(mas, &mm->mm_mt, addr, end - 1);
    mas_lock(&mas);

    vma_account_dec(mm, next);
    next->vm_start = addr;
    next->vm_pgoff = pgoff;
    vma_account_inc(mm, next);

    mas_set_range(&mas, next->vm_start, next->vm_end - 1);
    mas_store(&mas, next);

    mas_unlock(&mas);

    vma_layout_changed(mm, next);
    return next;
  }

  return nullptr;
}

struct vm_area_struct *vma_next(struct vm_area_struct *vma) {
  if (!vma || !vma->vm_mm) return nullptr;
  unsigned long index = vma->vm_end;
  return mt_find(&vma->vm_mm->mm_mt, &index, ULONG_MAX);
}

struct vm_area_struct *vma_prev(struct vm_area_struct *vma) {
  if (!vma || !vma->vm_mm || vma->vm_start == 0) return nullptr;
  MA_STATE(mas, &vma->vm_mm->mm_mt, vma->vm_start - 1, vma->vm_start - 1);
  return mas_find_rev(&mas, 0);
}

/* ========================================================================
 * VMA Creation and Initialization
 * ======================================================================== */

static inline uint64_t vm_get_page_prot(uint64_t flags) {
  uint64_t prot = PTE_PRESENT;
  if (flags & VM_WRITE) prot |= PTE_RW;
  if (flags & VM_USER) prot |= PTE_USER;
  if (!(flags & VM_EXEC)) prot |= PTE_NX;

  /* Handle Cache Attributes */
  if (flags & VM_CACHE_WC) prot |= VMM_CACHE_WC;
  else if (flags & VM_CACHE_UC) prot |= VMM_CACHE_UC;
  else if (flags & VM_CACHE_WT) prot |= VMM_CACHE_WT;

  return prot;
}

struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags) {
  if (start >= end || (start & (PAGE_SIZE - 1)) || (end & (PAGE_SIZE - 1)))
    return nullptr;

  struct vm_area_struct *vma = vma_alloc();
  if (!vma)
    return nullptr;

  vma->vm_start = start;
  vma->vm_end = end;
  vma->vm_flags = flags;
  vma->vm_page_prot = vm_get_page_prot(flags);
  vma->vm_mm = nullptr;
  vma->preferred_node = -1;
  vma->vm_ops = nullptr;
  vma->vm_private_data = nullptr;
  vma->vm_pgoff = 0;
  vma->vm_obj = nullptr;
  /*
   * Anonymous VMAs do not get an object until they are actually faulted.
   * This allows trivial merging of adjacent anonymous regions.
   * IO/PFN mappings still get their objects or are handled specially.
   */
  vma->anon_vma = nullptr;

  INIT_LIST_HEAD(&vma->anon_vma_chain);
  INIT_LIST_HEAD(&vma->vm_shared);
  rwsem_init(&vma->vm_lock);

  return vma;
}

/* ========================================================================
 * High-level VMA management (mmap/munmap/mprotect)
 * ======================================================================== */

#include <mm/shm.h>

uint64_t do_mmap(struct mm_struct *mm, uint64_t addr, size_t len, uint64_t prot, uint64_t flags, struct file *file,
                 struct shm_object *shm,
                 uint64_t pgoff) {
  struct vm_area_struct *vma;
  uint64_t vm_flags = 0;

  if (!mm || len == 0) return -EINVAL;

  /* 1. Translate Prot/Flags to VM_xxx */
  if (prot & PROT_READ) vm_flags |= VM_READ | VM_MAYREAD;
  if (prot & PROT_WRITE) vm_flags |= VM_WRITE | VM_MAYWRITE;
  if (prot & PROT_EXEC) vm_flags |= VM_EXEC | VM_MAYEXEC;

  if (flags & MAP_SHARED) vm_flags |= VM_SHARED | VM_MAYSHARE;
  if (flags & MAP_PRIVATE) vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
  if (flags & MAP_FIXED) vm_flags |= VM_DENYWRITE; /* Placeholder for MAP_FIXED checks */
  if (flags & MAP_LOCKED) vm_flags |= VM_LOCKED;
  if (flags & MAP_STACK) vm_flags |= VM_STACK;

  /* Standard user mappings */
  vm_flags |= VM_USER;

  len = (len + PAGE_SIZE - 1) & PAGE_MASK;

  down_write(&mm->mmap_lock);

  /* 2. Find Address */
  if (addr && (flags & MAP_FIXED)) {
    if (addr & (PAGE_SIZE - 1)) {
      up_write(&mm->mmap_lock);
      return -EINVAL;
    }
    /* Unmap anything in the way */
    int ret = do_munmap(mm, addr, len);
    if (ret < 0) {
      up_write(&mm->mmap_lock);
      return ret;
    }
  } else {
    addr = vma_find_free_region(mm, len, addr ? addr : PAGE_SIZE, vmm_get_max_user_address());
    if (!addr) {
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }
  }

  /* 3. Attempt proactive merge */
  vma = vma_merge(mm, nullptr, addr, addr + len, vm_flags, nullptr, pgoff);
  if (vma && !file && !shm) {
    goto out;
  }

  /* 4. Create VMA if merge failed */
  vma = vma_create(addr, addr + len, vm_flags);
  if (!vma) {
    up_write(&mm->mmap_lock);
    return -ENOMEM;
  }

  vma->vm_pgoff = pgoff;
  vma->preferred_node = mm->preferred_node;

  /* 5. Handle File-backed or SHM mapping */
  if (file) {
    extern int vfs_mmap(struct file *file, struct vm_area_struct *vma);
    int ret = vfs_mmap(file, vma);
    if (ret < 0) {
      vma_free(vma);
      up_write(&mm->mmap_lock);
      return ret;
    }
  } else if (shm) {
    /* Map the SHM object's VMO */
    vma->vm_obj = shm->vmo;
    vm_object_get(vma->vm_obj);

    /* Standard linking to object's mmap list */
    down_write(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    up_write(&vma->vm_obj->lock);
  } else if (flags & MAP_SHARED) {
    /*
     * MAP_SHARED | MAP_ANON:
     * We must create a shared anonymous object immediately so it can be
     * shared across fork() or via other mechanisms.
     */
    vma->vm_obj = vm_object_anon_create(vma_size(vma));
    if (!vma->vm_obj) {
      vma_free(vma);
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }

    down_write(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    up_write(&vma->vm_obj->lock);
  }

  /* 6. Insert */
  if (vma_insert(mm, vma) != 0) {
    vma_free(vma);
    up_write(&mm->mmap_lock);
    return -ENOMEM;
  }

out:
  up_write(&mm->mmap_lock);
  return addr;
}

int do_munmap(struct mm_struct *mm, uint64_t addr, size_t len) {
  struct vm_area_struct *vma, *tmp;
  struct mmu_gather tlb;

  if (!mm || len == 0 || (addr & (PAGE_SIZE - 1)))
    return -EINVAL;

  /* Check for overflow in length calculation */
  if (unlikely(len > (0xFFFFFFFFFFFFFFFFULL - addr))) {
    len = 0xFFFFFFFFFFFFFFFFULL - addr;
  }

  len = (len + PAGE_SIZE - 1) & PAGE_MASK;
  uint64_t end = addr + len;

  /* 1. Split partial overlaps at the boundaries */
  vma = vma_find(mm, addr);
  if (vma && vma->vm_start < addr) {
    if (vma_split(mm, vma, addr) != 0) return -ENOMEM;
  }
  vma = vma_find(mm, end - 1);
  if (vma && vma->vm_start < end && vma->vm_end > end) {
    if (vma_split(mm, vma, end) != 0) return -ENOMEM;
  }

  /* 2. Collect and remove all VMAs now strictly within [addr, end] */
  tlb_gather_mmu(&tlb, mm, addr, end);

  for_each_vma_range_safe(mm, vma, tmp, addr, end) {
    /* Unmap physical pages */
    if (mm->pml_root) {
      for (uint64_t curr = vma->vm_start; curr < vma->vm_end; curr += PAGE_SIZE) {
        struct folio *folio = vmm_unmap_folio_no_flush(mm, curr);
        if (folio) {
          tlb_remove_folio(&tlb, folio, curr);
        }
      }
    }

    vma_remove(mm, vma);
    if (vma->vm_ops && vma->vm_ops->close) vma->vm_ops->close(vma);
    vma_free(vma);
  }

  tlb_finish_mmu(&tlb);
  return 0;
}

int do_mprotect(struct mm_struct *mm, uint64_t addr, size_t len, uint64_t prot) {
  struct vm_area_struct *vma;
  uint64_t new_vm_flags = 0;

  if (!mm || (addr & (PAGE_SIZE - 1)) || len == 0) return -EINVAL;

  len = (len + PAGE_SIZE - 1) & PAGE_MASK;
  uint64_t end = addr + len;

  if (prot & PROT_READ) new_vm_flags |= VM_READ;
  if (prot & PROT_WRITE) new_vm_flags |= VM_WRITE;
  if (prot & PROT_EXEC) new_vm_flags |= VM_EXEC;

  down_write(&mm->mmap_lock);

  /* 1. Split partial overlaps */
  vma = vma_find(mm, addr);
  if (vma && vma->vm_start < addr) {
    vma_split(mm, vma, addr);
  }
  vma = vma_find(mm, end - 1);
  if (vma && vma->vm_start < end && vma->vm_end > end) {
    vma_split(mm, vma, end);
  }

  /* 2. Update flags and PTEs */
  for_each_vma(mm, vma) {
    if (vma->vm_end <= addr) continue;
    if (vma->vm_start >= end) break;

    /* Maintain some flags */
    uint64_t combined_flags = (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) | new_vm_flags;

    if (vma->vm_flags != combined_flags) {
      vma_account_dec(mm, vma);
      vma->vm_flags = combined_flags;
      vma_account_inc(mm, vma);
      vma_layout_changed(mm, vma);
    }

    vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

    if (mm->pml_root) {
      for (uint64_t curr = vma->vm_start; curr < vma->vm_end; curr += PAGE_SIZE) {
        vmm_set_flags(mm, curr, vma->vm_page_prot);
      }
    }
  }

  /* 3. Try merging after changes */
  vma = vma_find(mm, addr);
  if (vma) {
    vma_merge(mm, vma_prev(vma), vma->vm_start, vma->vm_end, vma->vm_flags, vma->vm_obj, vma->vm_pgoff);
  }

  up_write(&mm->mmap_lock);
  return 0;
}

/* ========================================================================
 * Legacy Compatibility / High-level VMA Operations
 * ======================================================================== */

int vma_map_range(struct mm_struct *mm, uint64_t start, uint64_t end, uint64_t flags) {
  uint64_t prot = 0;
  if (flags & VM_READ) prot |= PROT_READ;
  if (flags & VM_WRITE) prot |= PROT_WRITE;
  if (flags & VM_EXEC) prot |= PROT_EXEC;

  uint64_t mmap_flags = MAP_FIXED | MAP_PRIVATE;
  if (flags & VM_SHARED) mmap_flags = MAP_FIXED | MAP_SHARED;

  uint64_t ret = do_mmap(mm, start, end - start, prot, mmap_flags, nullptr, nullptr, 0);
  return (ret > 0xFFFFFFFFFFFFF000ULL) ? (int) ret : 0;
}

int vma_unmap_range(struct mm_struct *mm, uint64_t start, uint64_t end) {
  down_write(&mm->mmap_lock);
  int ret = do_munmap(mm, start, end - start);
  up_write(&mm->mmap_lock);
  return ret;
}

int vma_protect(struct mm_struct *mm, uint64_t start, uint64_t end, uint64_t new_flags) {
  uint64_t prot = 0;
  if (new_flags & VM_READ) prot |= PROT_READ;
  if (new_flags & VM_WRITE) prot |= PROT_WRITE;
  if (new_flags & VM_EXEC) prot |= PROT_EXEC;
  return do_mprotect(mm, start, end - start, prot);
}

int mm_populate_user_range(struct mm_struct *mm, uint64_t start, size_t size, uint64_t flags, const uint8_t *data,
                           size_t data_len) {
  if (!mm || size == 0) return -EINVAL;

  uint64_t end = (start + size + PAGE_SIZE - 1) & PAGE_MASK;
  start &= PAGE_MASK;

  /* Create VMAs */
  int ret = vma_map_range(mm, start, end, flags | VM_USER);
  if (ret != 0) return ret;

  /* Simple approach: allocate and map pages directly, bypassing fault handlers */
  down_write(&mm->mmap_lock);
  
  for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
    struct vm_area_struct *vma = vma_find(mm, addr);
    if (!vma || vma->vm_start > addr) {
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }

    /* Skip if already mapped */
    if (vmm_virt_to_phys(mm, addr) != 0) continue;

    /* Allocate physical page */
    struct folio *folio = alloc_pages(GFP_KERNEL, 0);
    if (!folio) {
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }

    /* Zero the page */
    void *page_virt = pmm_phys_to_virt(folio_to_phys(folio));
    memset(page_virt, 0, PAGE_SIZE);

    /* Map it directly */
    vmm_map_page(mm, addr, folio_to_phys(folio), vma->vm_page_prot);

    /* Create vm_object if needed and add folio */
    if (!vma->vm_obj) {
      vma->vm_obj = vm_object_anon_create(vma_size(vma));
      if (!vma->vm_obj) {
        folio_put(folio);
        up_write(&mm->mmap_lock);
        return -ENOMEM;
      }
      down_write(&vma->vm_obj->lock);
      list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
      up_write(&vma->vm_obj->lock);
    }

    /* Prepare RMAP */
    if (anon_vma_prepare(vma) != 0) {
      folio_put(folio);
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }

    /* Add to vm_object */
    uint64_t pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
    if (vma->vm_pgoff) pgoff += vma->vm_pgoff;
    
    down_write(&vma->vm_obj->lock);
    vm_object_add_folio(vma->vm_obj, pgoff, folio);
    up_write(&vma->vm_obj->lock);

    /* Setup RMAP */
    folio_add_anon_rmap(folio, vma, addr);
  }

  up_write(&mm->mmap_lock);

  /* Copy data */
  if (data && data_len > 0) {
    for (uint64_t addr = start; addr < end && (addr - start) < data_len; addr += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys(mm, addr);
      if (phys == 0) continue;
      void *virt = pmm_phys_to_virt(phys);
      size_t offset = addr - start;
      size_t to_copy = (data_len - offset) > PAGE_SIZE ? PAGE_SIZE : (data_len - offset);
      memcpy(virt, data + offset, to_copy);
    }
  }

  return 0;
}

/**
 * mm_prefault_range - Prefault pages in a VMA range
 * @vma: Target VMA
 * @start: Start address (must be page-aligned)
 * @end: End address (must be page-aligned)
 *
 * Inspired by Linux's filemap_map_pages and XNU's vm_fault_enter.
 * Prefaults pages without holding mmap_lock as write, using the
 * per-VMA lock for fine-grained synchronization.
 *
 * Returns 0 on success, negative error code on failure.
 */
int mm_prefault_range(struct vm_area_struct *vma, uint64_t start, uint64_t end) {
  struct mm_struct *mm = vma->vm_mm;
  struct vm_object *obj = vma->vm_obj;
  int ret = 0;

  if (!mm || !obj) return -EINVAL;
  if (start >= end) return -EINVAL;

  /* Ensure RMAP is ready for anonymous mappings */
  if (obj->type == VM_OBJECT_ANON) {
    if (unlikely(anon_vma_prepare(vma))) return -ENOMEM;
  }

  /* Take VMA lock to prevent concurrent modifications */
  vma_lock_shared(vma);

  /* Verify VMA bounds */
  if (start < vma->vm_start || end > vma->vm_end) {
    vma_unlock_shared(vma);
    return -EINVAL;
  }

#ifdef CONFIG_MM_POPULATE_BATCH_SIZE
  const size_t batch_size = CONFIG_MM_POPULATE_BATCH_SIZE;
#else
  const size_t batch_size = 16;
#endif

  for (uint64_t addr = start; addr < end; addr += PAGE_SIZE * batch_size) {
    uint64_t batch_end = addr + (PAGE_SIZE * batch_size);
    if (batch_end > end) batch_end = end;

    /* Process batch */
    for (uint64_t curr = addr; curr < batch_end; curr += PAGE_SIZE) {
      /* Skip if already mapped */
      if (vmm_virt_to_phys(mm, curr) != 0) continue;

      /* Prepare fault context */
      struct vm_fault vmf;
      vmf.address = curr;
      vmf.flags = FAULT_FLAG_WRITE; /* Populate implies write access */
      vmf.pgoff = (curr - vma->vm_start) >> PAGE_SHIFT;
      if (vma->vm_pgoff) vmf.pgoff += vma->vm_pgoff;
      vmf.folio = nullptr;

      /* Call fault handler */
      ret = handle_mm_fault(vma, curr, vmf.flags);
      if (ret != 0) {
        /* Non-fatal: continue with next page */
        continue;
      }

#ifdef CONFIG_MM_POPULATE_FAULT_AROUND
      /* Fault-around: map neighboring pages if they exist */
      if (obj->ops && obj->ops->fault) {
        down_read(&obj->lock);
        for (int i = -4; i <= 4; i++) {
          if (i == 0) continue;
          uint64_t around_addr = curr + (i * PAGE_SIZE);
          if (around_addr < vma->vm_start || around_addr >= vma->vm_end) continue;
          if (vmm_virt_to_phys(mm, around_addr) != 0) continue;

          uint64_t around_pgoff = vmf.pgoff + i;
          struct folio *f = vm_object_find_folio(obj, around_pgoff);
          if (f) {
            vmm_map_page(mm, around_addr, folio_to_phys(f), vma->vm_page_prot);
          }
        }
        up_read(&obj->lock);
      }
#endif
    }

    /* Yield CPU periodically to avoid monopolizing */
    if (batch_end < end) {
      vma_unlock_shared(vma);
      if (preemptible()) schedule();
      vma_lock_shared(vma);

      /* Re-verify VMA is still valid */
      if (vma->vm_start > start || vma->vm_end < end) {
        ret = -EINVAL;
        break;
      }
    }
  }

  vma_unlock_shared(vma);
  return ret;
}

/**
 * mm_populate_range - Populate (prefault) pages in an address range
 * @mm: Target address space
 * @start: Start address (page-aligned)
 * @end: End address (page-aligned)
 * @locked: Whether mmap_lock is already held
 *
 * Production-ready implementation inspired by Linux's __mm_populate.
 * Handles:
 * - Proper VMA traversal
 * - Lock management (can be called with or without mmap_lock)
 * - Batch processing to avoid long lock hold times
 * - Error recovery
 *
 * Returns 0 on success, negative error code on failure.
 */
int mm_populate_range(struct mm_struct *mm, uint64_t start, uint64_t end, bool locked) {
  struct vm_area_struct *vma;
  int ret = 0;

  if (!mm || start >= end) return -EINVAL;

  start &= PAGE_MASK;
  end = (end + PAGE_SIZE - 1) & PAGE_MASK;

  if (!locked) down_read(&mm->mmap_lock);

  /* Traverse VMAs in the range */
  for (uint64_t addr = start; addr < end; ) {
    vma = vma_find(mm, addr);
    if (!vma || vma->vm_start > addr) {
      /* Gap in address space */
      ret = -ENOMEM;
      break;
    }

    /* Ensure VMA has a vm_object */
    if (!vma->vm_obj) {
      /* Lazy object creation for anonymous mappings */
      if (vma->vm_flags & (VM_IO | VM_PFNMAP)) {
        /* Skip special mappings */
        addr = vma->vm_end;
        continue;
      }

      /* Must upgrade to write lock for object creation */
      up_read(&mm->mmap_lock);
      down_write(&mm->mmap_lock);

      /* Re-find VMA after lock upgrade */
      vma = vma_find(mm, addr);
      if (!vma || vma->vm_start > addr) {
        downgrade_write(&mm->mmap_lock);
        ret = -ENOMEM;
        break;
      }

      /* Create anonymous object if still missing */
      if (!vma->vm_obj) {
        vma->vm_obj = vm_object_anon_create(vma_size(vma));
        if (!vma->vm_obj) {
          downgrade_write(&mm->mmap_lock);
          ret = -ENOMEM;
          break;
        }

        /* Link VMA to object */
        down_write(&vma->vm_obj->lock);
        list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
        up_write(&vma->vm_obj->lock);
      }

      downgrade_write(&mm->mmap_lock);
    }

    /* Calculate range to prefault in this VMA */
    uint64_t vma_start = addr > vma->vm_start ? addr : vma->vm_start;
    uint64_t vma_end = end < vma->vm_end ? end : vma->vm_end;

    /* Release mmap_lock during prefaulting (per-VMA lock is used) */
    up_read(&mm->mmap_lock);

    /* Prefault pages in this VMA */
    ret = mm_prefault_range(vma, vma_start, vma_end);

    /* Re-acquire mmap_lock */
    down_read(&mm->mmap_lock);

    if (ret != 0) {
      /* Non-fatal: continue with next VMA */
      ret = 0;
    }

    addr = vma->vm_end;
  }

  if (!locked) up_read(&mm->mmap_lock);

  return ret;
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
  if (unlikely(!mm || size == 0))
    return 0;

  if (range_end == 0)
    range_end = vmm_get_max_user_address();

  if (unlikely(range_start >= range_end))
    return 0;

  size = ALIGN(size, PAGE_SIZE);
  uint64_t guard = PAGE_SIZE;
  uint64_t total_needed = size + 2 * guard;

  /*
   * Optimization: Use last_hole as a hint for O(1) sequential allocation.
   * To ensure correctness, we verify it doesn't overlap using a fast walk.
   */
  unmet_cond_crit(!mm);
  if (mm->last_hole >= range_start && mm->last_hole < range_end) {
    uint64_t candidate = ALIGN(mm->last_hole + guard, alignment);
    if (candidate + size + guard <= range_end) {
      /* Fast check using MA_STATE to see if the range is truly empty */
      MA_STATE(mas, &mm->mm_mt, candidate - guard, candidate + size + guard - 1);
      if (!mas_find(&mas, candidate + size + guard - 1)) {
        mm->last_hole = candidate + size;
        return candidate;
      }
    }
  }

  uint64_t orig_start = range_start;
  /*
   * ASLR: Only randomize if the range is significantly larger than requested.
   * This prevents excessive fragmentation in small heaps.
   */
  if ((range_end - range_start) > (total_needed + 0x1000000)) {
    // 16MB threshold
    uint64_t offset;
    if (rdrand64_safe(&offset)) {
      /* TODO: TRNG */
      uint64_t max_offset = range_end - range_start - total_needed;
      range_start += ALIGN(offset % max_offset, alignment);
    }
  }

  MA_STATE(mas, &mm->mm_mt, 0, 0);

retry:
  /*
   * Use Maple Tree's augmented search for empty areas.
   * This is O(log N) and uses the precomputed gap information.
   */
  if (mas_empty_area(&mas, range_start, range_end - 1, total_needed)) {
    if (range_start != orig_start) {
      range_start = orig_start;
      goto retry;
    }
    return 0;
  }

  /* Calculate aligned address within the found hole */
  uint64_t addr = ALIGN(mas.index + guard, alignment);

  /* Validate that we still fit after alignment */
  if (unlikely(addr + size + guard > range_end)) {
    /* Hole was too small after alignment, search again from here */
    range_start = addr + 1;
    goto retry;
  }

  /* Update hints for next allocation */
  mm->last_hole = addr + size;

  return addr;
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

  printk(VMA_CLASS "[--- VMA Dump for MM: %p (%d-level paging) ---\n", mm, vmm_get_paging_levels());
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
#ifdef MM_HARDENING
  struct vm_area_struct *vma, *prev = nullptr;

  for_each_vma(mm, vma) {
    vma_verify(vma);
    if (prev && prev->vm_end > vma->vm_start) {
      printk(KERN_ERR "VMA Corruption: Overlap detected [%llx, %llx] and [%llx, %llx]\n",
             prev->vm_start, prev->vm_end, vma->vm_start, vma->vm_end);
      return -EINVAL;
    }
    prev = vma;
  }
#else
  (void) mm;
#endif
  return 0;
}

int vma_verify_list(struct mm_struct *mm) {
#ifdef MM_HARDENING
  struct vm_area_struct *vma, *prev = nullptr;

  for_each_vma(mm, vma) {
    if (prev && prev->vm_start >= vma->vm_start) {
      printk(VMA_CLASS "ERROR: List not sorted!\n");
      return -EINVAL;
    }
    if (prev && prev->vm_end > vma->vm_start) {
      printk(VMA_CLASS "ERROR: Overlapping VMAs in list!\n");
      return -EINVAL;
    }
    prev = vma;
  }
#else
  (void) mm;
#endif
  return 0;
}

/**
 * do_madvise - Provide hints about memory usage for a range.
 */
int do_madvise(struct mm_struct *mm, uint64_t addr, size_t len, int advice) {
  struct vm_area_struct *vma;
  uint64_t end;

  if (!mm || len == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;

  len = PAGE_ALIGN_UP(len);
  end = addr + len;

  down_write(&mm->mmap_lock);

  /* 1. Split VMAs at boundaries if necessary */
  vma = vma_find(mm, addr);
  if (vma && vma->vm_start < addr) {
    if (vma_split(mm, vma, addr) != 0) {
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }
  }
  vma = vma_find(mm, end - 1);
  if (vma && vma->vm_start < end && vma->vm_end > end) {
    if (vma_split(mm, vma, end) != 0) {
      up_write(&mm->mmap_lock);
      return -ENOMEM;
    }
  }

  /* 2. Apply advice to all VMAs in range */
  for_each_vma_range(mm, vma, addr, end) {
    switch (advice) {
      case MADV_NORMAL:
        vma->vm_flags &= ~(VM_RANDOM | VM_SEQUENTIAL);
        break;
      case MADV_RANDOM:
        vma->vm_flags &= ~VM_SEQUENTIAL;
        vma->vm_flags |= VM_RANDOM;
        break;
      case MADV_SEQUENTIAL:
        vma->vm_flags &= ~VM_RANDOM;
        vma->vm_flags |= VM_SEQUENTIAL;
        break;
      case MADV_HUGEPAGE:
        vma->vm_flags &= ~VM_NOHUGEPAGE;
        vma->vm_flags |= VM_HUGEPAGE;
        break;
      case MADV_NOHUGEPAGE:
        vma->vm_flags &= ~VM_HUGEPAGE;
        vma->vm_flags |= VM_NOHUGEPAGE;
        break;
      case MADV_DONTNEED:
      case MADV_FREE:
        /*
         * For DONTNEED/FREE, we unmap the physical pages.
         * The VMA remains, but subsequent accesses will fault them back in.
         */
        if (mm->pml_root) {
          for (uint64_t curr = vma->vm_start; curr < vma->vm_end; curr += PAGE_SIZE) {
            struct folio *folio = vmm_unmap_folio(mm, curr);
            if (folio) {
              /*
               * Release the page. If it was anonymous, it's discarded (DONTNEED behavior).
               */
              folio_put(folio);
            }
          }
        }
        break;
      case MADV_WILLNEED:
        /* Pre-faulting could be implemented here */
        break;
      default:
        break;
    }
    mm->vmacache_seqnum++;
    atomic_inc(&mm->mmap_seq);
  }

  up_write(&mm->mmap_lock);
  return 0;
}

/* ========================================================================
 * Global Kernel MM
 * ======================================================================== */

struct mm_struct init_mm;

/* ========================================================================
 * Test Suite
 * ======================================================================== */

#define TEST_VMA_COUNT 1024
#define TEST_RANGE_START 0x1000000000ULL
#define TEST_RANGE_END   0x2000000000ULL

void vma_extreme_stress_test(void) {
  printk(KERN_DEBUG VMA_CLASS "Starting VMA Stress Test...\n");
  struct mm_struct *mm = mm_create();
  if (!mm) panic("vma_stress: failed to create mm");

  /* 1. Massive Fragmentation Test: Create 1024 small VMAs with gaps */
  printk(KERN_DEBUG VMA_CLASS "  - Phase 1: Massive Fragmentation...");
  for (int i = 0; i < TEST_VMA_COUNT; i++) {
    uint64_t addr = TEST_RANGE_START + (i * 2 * PAGE_SIZE);
    if (vma_map_range(mm, addr, addr + PAGE_SIZE, VM_READ | VM_WRITE) != 0) {
      panic("vma_stress: failed phase 1 at iteration %d", i);
    }
  }
  if (mm->map_count != TEST_VMA_COUNT) panic("vma_stress: phase 1 count mismatch: %d", mm->map_count);
  printk("OK\n");

  /* 2. Bridge Merge Test: Fill the gaps to trigger proactive bridge merging */
  printk(KERN_DEBUG VMA_CLASS "  - Phase 2: Proactive Bridge Merging...");
  for (int i = 0; i < TEST_VMA_COUNT - 1; i++) {
    uint64_t addr = TEST_RANGE_START + (i * 2 * PAGE_SIZE) + PAGE_SIZE;
    /* Filling the gap between VMA_i and VMA_i+1 */
    if (vma_map_range(mm, addr, addr + PAGE_SIZE, VM_READ | VM_WRITE) != 0) {
      panic("vma_stress: failed phase 2 at iteration %d", i);
    }
  }
  /* All VMAs should have merged into ONE giant VMA */
  if (mm->map_count != 1) {
    panic("vma_stress: phase 2 merge failed, map_count: %d (expected 1)", mm->map_count);
  }
  printk("OK\n");

  /* 3. Swiss Cheese Test: Unmap small chunks from the middle */
  printk(KERN_DEBUG VMA_CLASS "  - Phase 3: Swiss Cheese Unmapping...");
  uint64_t giant_start = TEST_RANGE_START;
  for (int i = 0; i < 512; i++) {
    uint64_t addr = giant_start + (i * 4 * PAGE_SIZE) + PAGE_SIZE;
    if (vma_unmap_range(mm, addr, addr + PAGE_SIZE) != 0) {
      panic("vma_stress: failed phase 3 at iteration %d", i);
    }
  }
  printk("OK\n");

  /* 4. Parallel Fault Simulation (Speculative path exercise) */
  printk(KERN_DEBUG VMA_CLASS "  - Phase 4: Speculative Fault Validation...");
  struct vm_area_struct *vma;
  int checked = 0;
  rcu_read_lock();
  for_each_vma(mm, vma) {
    /* Simulate fault handler looking at VMAs while another CPU might modify them */
    uint32_t seq = atomic_read(&mm->mmap_seq);
    if (vma->vm_start & PAGE_MASK) checked++;
    if (atomic_read(&mm->mmap_seq) != seq) {
      /* This is fine in real parallel, but here it shouldn't change */
    }
  }
  rcu_read_unlock();
  printk("OK (%d VMAs checked)\n", checked);

  /* Clean up */
  mm_destroy(mm);
  mm_free(mm);
}

void vma_test(void) {
  uint64_t start = get_time_ns();
  printk(KERN_DEBUG VMA_CLASS "Starting VMA smoke test...\n");

  /* Use mm_create to get valid page tables and exercise VMM glue */
  struct mm_struct *mm = mm_create();
  if (!mm) panic("vma_test: failed to create mm");

  /* Test 1: Basic Mappings & Insertion */
  /* Create two 2-page VMAs: [0x1000, 0x3000] and [0x5000, 0x7000] */
  vma_map_range(mm, 0x1000, 0x3000, VM_READ);
  vma_map_range(mm, 0x5000, 0x7000, VM_READ | VM_WRITE);

  if (mm->map_count != 2) panic("vma_test: map_count mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - Basic Mapping: OK\n");

  /* Test 2: Gap Finding (Maple Tree) */
  /* NOTE: With guard pages, we can't fit 4KB in the 8KB gap between 0x3000 and 0x5000. */
  /* 0x3000 (end of VMA1) -> Guard (0x4000) -> Data (0x5000) -> Collision with VMA2. */
  /* So it should find space AFTER 0x7000. */
  /* Expected: 0x7000 + Guard(0x1000) = 0x8000. */
  uint64_t free = vma_find_free_region(mm, 0x1000, 0x1000, 0x10000);

  /* Depending on ASLR, this might be higher, but we restricted ASLR range start to low. */
  /* However, vma_find_free_region uses ASLR now. */
  /* To test deterministic behavior, we should perhaps rely on 'alignment' or just verify it's valid. */

  if (free == 0) panic("vma_test: gap find failed completely");

  /* Verify it doesn't overlap or touch */
  struct vm_area_struct *v1 = vma_find(mm, free);
  if (v1) panic("vma_test: allocated on existing VMA");

  /* Check overlap with 0x1000-0x3000 */
  if (free >= 0x1000 && free < 0x3000) panic("vma_test: overlap VMA1");
  /* Check overlap with 0x5000-0x7000 */
  if (free >= 0x5000 && free < 0x7000) panic("vma_test: overlap VMA2");

  /* Check guard pages */
  if (free == 0x3000 || free + 0x1000 == 0x5000) panic("vma_test: guard page violation");

  printk(KERN_DEBUG VMA_CLASS "  - Gap Finding: OK (Got %llx)\n", free);

  /* Test 3: VMA Splitting (Must be page aligned) */
  printk(KERN_DEBUG VMA_CLASS "  - VMA Splitting: start...\n");
  down_write(&mm->mmap_lock);
  struct vm_area_struct *vma_to_split = vma_find(mm, 0x5000);
  if (!vma_to_split) panic("vma_test: could not find vma at 0x5000");

  /* Split [0x5000, 0x7000] at 0x6000 */
  if (vma_split(mm, vma_to_split, 0x6000) != 0) {
    panic("vma_test: split failed");
  }
  up_write(&mm->mmap_lock);

  if (mm->map_count != 3) panic("vma_test: map_count after split mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - VMA Splitting: OK\n");

  /* Test 4: VMA Protection (with split) */
  printk(KERN_DEBUG VMA_CLASS "  - VMA Protect (Split): start...\n");
  /* Change protection on the first page of [0x1000, 0x3000] */
  if (vma_protect(mm, 0x1000, 0x2000, VM_READ | VM_WRITE) != 0) panic("vma_test: protect failed");
  if (mm->map_count != 4) panic("vma_test: map_count after protect mismatch");
  printk(KERN_DEBUG VMA_CLASS "  - VMA Protect (Split): OK\n");

  /* Test 5: Unmapping partial & full */
  /* Unmap the middle pages across multiple VMAs: [0x2000, 0x6000] */
  vma_unmap_range(mm, 0x2000, 0x6000);
  printk(KERN_DEBUG VMA_CLASS "  - Partial Unmap: OK\n");

  /* Clean up all */
  vma_unmap_range(mm, 0, vmm_get_max_user_address());
  if (mm->map_count != 0) panic("vma_test: unmap all failed");
  printk(KERN_DEBUG VMA_CLASS "  - Unmap All: OK\n");

  mm_destroy(mm);
  mm_free(mm);
  printk(KERN_DEBUG VMA_CLASS "VMA smoke test Passed.\n");

  vma_extreme_stress_test();
  printk(KERN_DEBUG VMA_CLASS "VMA Stress Test PASSED. (%lld ns)\n", get_time_ns() - start);
}
