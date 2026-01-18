/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/memory.c
 * @brief High-level memory management
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

#include <mm/mm_types.h>
#include <mm/zone.h>
#include <mm/vma.h>
#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <mm/mmu_gather.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/mutex.h>
#include <linux/container_of.h>
#include <linux/list.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <aerosync/wait.h>
#include <aerosync/sched/process.h>

/* Global LRU Lists - Per-CPU to reduce cache line bouncing */
DEFINE_PER_CPU(struct list_head, inactive_list);
DEFINE_PER_CPU(struct list_head, active_list);
DEFINE_PER_CPU(spinlock_t, lru_lock);

/**
 * struct scan_control - Control parameters for a reclamation pass.
 */
struct scan_control {
  size_t nr_to_reclaim;
  gfp_t gfp_mask;
  int priority; /* 0 (max) to 12 (min) */
  size_t nr_reclaimed;
  size_t nr_scanned;
};

/**
 * folio_add_lru - Add a folio to the inactive LRU list.
 */
void folio_add_lru(struct folio *folio) {
  if (!folio) return;

  struct list_head *inactive = this_cpu_ptr(inactive_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);

  irq_flags_t flags = spinlock_lock_irqsave(lock);

  if (folio->flags & PG_lru) {
    spinlock_unlock_irqrestore(lock, flags);
    return;
  }

  list_add(&folio->lru, inactive);
  folio->flags |= PG_lru;

  spinlock_unlock_irqrestore(lock, flags);
}

/**
 * try_to_unmap_folio - The core of memory reclamation.
 * Unmaps a folio from all VMAs that reference it.
 */
int try_to_unmap_folio(struct folio *folio, struct mmu_gather *tlb) {
  void *mapping = folio->mapping;
  if (!mapping) return 0;

  rcu_read_lock();

  /* Check if it's anonymous (bit 0 set) */
  if ((uintptr_t) mapping & 0x1) {
    struct anon_vma *av = (struct anon_vma *) ((uintptr_t) mapping & ~0x1);

    // Validate anon_vma before use
    if (!av || atomic_read(&av->refcount) == 0) {
      folio->mapping = NULL;
      rcu_read_unlock();
      return 0;
    }

    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);

    // Re-check after acquiring lock
    if (atomic_read(&av->refcount) == 0) {
      spinlock_unlock_irqrestore(&av->lock, flags);
      folio->mapping = NULL;
      rcu_read_unlock();
      return 0;
    }

    struct anon_vma_chain *avc;

    list_for_each_entry(avc, &av->head, same_anon_vma) {
      struct vm_area_struct *vma = avc->vma;
      if (!vma || !vma->vm_mm) continue;

      uint64_t address = vma->vm_start + (folio->index << PAGE_SHIFT);
      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (tlb) {
          uint64_t phys = vmm_unmap_page_no_flush(vma->vm_mm, address);
          if (phys) {
            tlb->mm = vma->vm_mm; // Track last modified mm
            tlb_remove_folio(tlb, folio, address);
          }
        } else {
          vmm_unmap_page(vma->vm_mm, address);
        }
      }
    }

    spinlock_unlock_irqrestore(&av->lock, flags);
    folio->mapping = NULL;
    rcu_read_unlock();
    return 1;
  }

  /* Shared Memory / File-backed (bit 0 NOT set) */
  struct vm_object *obj = (struct vm_object *) mapping;
  down_read(&obj->lock);
  struct vm_area_struct *vma;

  list_for_each_entry(vma, &obj->i_mmap, vm_shared) {
    if (folio->index < vma->vm_pgoff) continue;

    uint64_t pgoff_in_vma = folio->index - vma->vm_pgoff;
    if (pgoff_in_vma >= vma_pages(vma)) continue;

    uint64_t address = vma->vm_start + (pgoff_in_vma << PAGE_SHIFT);

    if (vma->vm_mm->pml_root) {
      if (tlb) {
        uint64_t phys = vmm_unmap_page_no_flush(vma->vm_mm, address);
        if (phys) {
          tlb->mm = vma->vm_mm;
          tlb_remove_folio(tlb, folio, address);
        }
      } else {
        vmm_unmap_page(vma->vm_mm, address);
      }
    }
  }

  up_read(&obj->lock);
  folio->mapping = NULL;
  rcu_read_unlock();
  return 1;
}

/**
 * folio_referenced - Check if any PTE pointing to this folio has the Accessed bit set.
 */
int folio_referenced(struct folio *folio) {
  int referenced = 0;
  void *mapping = folio->mapping;
  if (!mapping) return 0;

  rcu_read_lock();

  if ((uintptr_t) mapping & 0x1) {
    struct anon_vma *av = (struct anon_vma *) ((uintptr_t) mapping & ~0x1);
    irq_flags_t flags = spinlock_lock_irqsave(&av->lock);
    struct anon_vma_chain *avc;

    list_for_each_entry(avc, &av->head, same_anon_vma) {
      struct vm_area_struct *vma = avc->vma;
      uint64_t address = vma->vm_start + (folio->index << PAGE_SHIFT);

      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (vmm_is_accessed(vma->vm_mm, address)) {
          referenced++;
          vmm_clear_accessed(vma->vm_mm, address);
        }
      }
    }
    spinlock_unlock_irqrestore(&av->lock, flags);
  } else {
    /* VM Object */
    struct vm_object *obj = (struct vm_object *) mapping;
    down_read(&obj->lock);
    struct vm_area_struct *vma;

    list_for_each_entry(vma, &obj->i_mmap, vm_shared) {
      uint64_t address = vma->vm_start + ((folio->index - vma->vm_pgoff) << PAGE_SHIFT);

      if (address < vma->vm_start || address >= vma->vm_end) continue;

      if (vma->vm_mm->pml_root) {
        if (vmm_is_accessed(vma->vm_mm, address)) {
          referenced++;
          vmm_clear_accessed(vma->vm_mm, address);
        }
      }
    }
    up_read(&obj->lock);
  }

  rcu_read_unlock();
  return referenced;
}

/**
 * folio_reclaim - Attempt to free a folio by unmapping it from all users.
 */
int folio_reclaim(struct folio *folio, struct mmu_gather *tlb) {
  if (folio_referenced(folio)) {
    /* Recently accessed, move to active list (if on inactive) */
    return -EAGAIN;
  }

  if (try_to_unmap_folio(folio, tlb)) {
    /* 
     * If TLB context is provided, we don't free pages immediately.
     * The TLB finish pass will free them after the shootdown.
     */
    if (!tlb) {
      pmm_free_pages(folio_to_phys(folio), folio_nr_pages(folio));
    }
    return 0;
  }

  return -1;
}

/**
 * shrink_active_list - Move pages from active to inactive if they aren't referenced.
 */
void shrink_active_list(size_t nr_to_scan, struct scan_control *sc) {
  struct list_head *active = this_cpu_ptr(active_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);
  struct list_head folio_list;
  INIT_LIST_HEAD(&folio_list);

  irq_flags_t flags = spinlock_lock_irqsave(lock);
  for (size_t i = 0; i < nr_to_scan && !list_empty(active); i++) {
    struct folio *folio = list_last_entry(active, struct folio, lru);
    list_move(&folio->lru, &folio_list);
    folio->flags &= ~PG_active;
  }
  spinlock_unlock_irqrestore(lock, flags);

  struct list_head *pos, *q;
  list_for_each_safe(pos, q, &folio_list) {
    struct folio *folio = list_entry(pos, struct folio, lru);

    if (folio_referenced(folio)) {
      /* Still active, move back to head of active list */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, active);
      folio->flags |= PG_active;
      spinlock_unlock_irqrestore(lock, flags);
    } else {
      /* Not referenced, move to inactive list */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, this_cpu_ptr(inactive_list));
      spinlock_unlock_irqrestore(lock, flags);
    }
    sc->nr_scanned++;
  }
}

/**
 * shrink_inactive_list - Scan the inactive LRU for pages to reclaim.
 */
size_t shrink_inactive_list(size_t nr_to_scan) {
  size_t reclaimed = 0;
  struct list_head folio_list;
  INIT_LIST_HEAD(&folio_list);

  struct list_head *inactive = this_cpu_ptr(inactive_list);
  spinlock_t *lock = this_cpu_ptr(lru_lock);

  irq_flags_t flags = spinlock_lock_irqsave(lock);
  for (size_t i = 0; i < nr_to_scan && !list_empty(inactive); i++) {
    struct folio *folio = list_last_entry(inactive, struct folio, lru);
    list_move(&folio->lru, &folio_list);
    folio->flags &= ~PG_lru;
  }
  spinlock_unlock_irqrestore(lock, flags);

  /*
   * Batch TLB shootdown for all pages unmapped during this pass.
   * This prevents "IPI Storms" on SMP.
   */
  struct mmu_gather tlb;
  tlb_gather_mmu(&tlb, &init_mm, 0, 0); // Dynamic range

  struct list_head *pos, *q;
  list_for_each_safe(pos, q, &folio_list) {
    struct folio *folio = list_entry(pos, struct folio, lru);

    int ret = folio_reclaim(folio, &tlb);
    if (ret == 0) {
      reclaimed++;
    } else if (ret == -EAGAIN) {
      /* Re-activate */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, this_cpu_ptr(active_list));
      folio->flags |= (PG_lru | PG_active);
      spinlock_unlock_irqrestore(lock, flags);
    } else {
      /* Re-insert into inactive */
      flags = spinlock_lock_irqsave(lock);
      list_add(&folio->lru, inactive);
      folio->flags |= PG_lru;
      spinlock_unlock_irqrestore(lock, flags);
    }
  }

  tlb_finish_mmu(&tlb);

  return reclaimed;
}

/**
 * shrink_zone - Standard reclamation for a single zone.
 */
static void shrink_zone(struct zone *zone, struct scan_control *sc) {
  /*
   * In a production kernel, we would scan proportional to list sizes.
   * Here we scan a fixed batch based on priority.
   */
  size_t nr_active = 32 >> (12 - sc->priority);
  size_t nr_inactive = 64 >> (12 - sc->priority);

  /* Aging */
  shrink_active_list(nr_active, sc);

  /* Reclamation */
  sc->nr_reclaimed += shrink_inactive_list(nr_inactive);
}

void wakeup_kswapd(struct zone *zone) {
  if (!zone || !zone->zone_pgdat) return;
  wake_up(&zone->zone_pgdat->kswapd_wait);
}

static int kswapd_should_run(struct pglist_data *pgdat) {
  for (int i = 0; i < MAX_NR_ZONES; i++) {
    struct zone *z = &pgdat->node_zones[i];
    if (z->present_pages > 0 && z->nr_free_pages < z->watermark[WMARK_HIGH])
      return 1;
  }
  return 0;
}

static int kswapd_thread(void *data) {
  struct pglist_data *pgdat = (struct pglist_data *)data;
  printk(KERN_INFO SWAP_CLASS "kswapd started for node %d\n", pgdat->node_id);

  while (1) {
    wait_event(&pgdat->kswapd_wait, kswapd_should_run(pgdat));

    struct scan_control sc = {
      .gfp_mask = GFP_KERNEL,
      .nr_to_reclaim = 128,
      .priority = 12, /* Start with lowest priority */
    };

    while (sc.priority >= 0) {
      for (int i = MAX_NR_ZONES - 1; i >= 0; i--) {
        struct zone *z = &pgdat->node_zones[i];
        if (z->present_pages == 0) continue;

        if (z->nr_free_pages < z->watermark[WMARK_HIGH]) {
          shrink_zone(z, &sc);
        }
      }

      if (sc.nr_reclaimed >= sc.nr_to_reclaim) break;

      /* If not enough reclaimed, increase pressure */
      sc.priority--;
    }
  }
  return 0;
}

void kswapd_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (node_data[n] && node_data[n]->node_spanned_pages > 0) {
       char name[16];
       snprintf(name, sizeof(name), "kswapd%d", n);
       struct task_struct *k = kthread_create(kswapd_thread, node_data[n], name);
       if (k) {
         node_data[n]->kswapd_task = k;
         kthread_run(k);
       }
    }
  }
}

void lru_init(void) {
  int cpu;
  for_each_possible_cpu(cpu) {
    struct list_head *inactive = per_cpu_ptr(inactive_list, cpu);
    struct list_head *active = per_cpu_ptr(active_list, cpu);
    spinlock_t *lock = per_cpu_ptr(lru_lock, cpu);

    INIT_LIST_HEAD(inactive);
    INIT_LIST_HEAD(active);
    spinlock_init(lock);
  }
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
  /*
   * Anonymous mapping: bit 0 is set.
   * If the folio is already mapped, we skip adding it to the LRU again,
   * but we ensure the mapping is correct (e.g. for COW).
   */
  if (!folio->mapping) {
    folio->mapping = (void *) ((uintptr_t) vma->anon_vma | 0x1);
    folio->index = (address - vma->vm_start) >> PAGE_SHIFT;
    folio_add_lru(folio);
  }
}

/**
 * folio_add_file_rmap - Links a physical folio to its owning VM Object.
 */
void folio_add_file_rmap(struct folio *folio, struct vm_object *obj, uint64_t pgoff) {
  if (!folio->mapping) {
    /* VM Object mapping: bit 0 is NOT set */
    folio->mapping = (void *) obj;
    folio->index = pgoff;
    folio_add_lru(folio);
  }
}

/* --- Shared Memory Support --- */

static int shmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
  if (!vma->vm_obj) return VM_FAULT_SIGBUS;
  vmf->prot = vma->vm_page_prot;
  return vma->vm_obj->ops->fault(vma->vm_obj, vma, vmf);
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
  if (vma->vm_pgoff) vmf.pgoff += vma->vm_pgoff;
  vmf.folio = NULL;

  int ret = VM_FAULT_SIGSEGV;

  /*
   * Priority 1: VMA Operations (Special/Legacy)
   * Check these first as they might override object behavior.
   */
  if (vma->vm_ops && vma->vm_ops->fault) {
    ret = vma->vm_ops->fault(vma, &vmf);
  }
  /*
   * Priority 2: VM Object (Production-ready MM)
   * The object owns the pages and handles its own backing store.
   */
  else if (vma->vm_obj && vma->vm_obj->ops && vma->vm_obj->ops->fault) {
    ret = vma->vm_obj->ops->fault(vma->vm_obj, vma, &vmf);
  }
  /*
   * Priority 3: Lazy Anonymous Object Creation
   * If no object exists and it's a standard mapping, create an anon object.
   */
  else if (!vma->vm_obj && !(vma->vm_flags & (VM_IO | VM_PFNMAP))) {
    if (flags & FAULT_FLAG_SPECULATIVE) return VM_FAULT_RETRY;

    vma->vm_obj = vm_object_anon_create(vma_size(vma));
    if (!vma->vm_obj) return VM_FAULT_OOM;

    /* Link to object */
    down_write(&vma->vm_obj->lock);
    list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
    up_write(&vma->vm_obj->lock);

    ret = vma->vm_obj->ops->fault(vma->vm_obj, vma, &vmf);
  }

  if (ret == VM_FAULT_COMPLETED) return 0;
  if (ret != 0) return ret;

  if (vmf.folio) {
    struct folio *folio = vmf.folio;
    uint64_t phys = folio_to_phys(folio);

    if (PageHead(&folio->page) && folio->page.order == 9) {
      vmm_map_huge_page(vma->vm_mm, vmf.address & 0xFFFFFFFFFFE00000ULL,
                        phys, vmf.prot, VMM_PAGE_SIZE_2M);
    } else {
      vmm_map_page(vma->vm_mm, vmf.address, phys, vmf.prot);
      
      /* 
       * FAULT-AROUND: Try to map neighboring pages if they are already in memory.
       * We expand the window to 16 pages (64KB) for better spatial locality.
       */
      if (!(flags & FAULT_FLAG_WRITE) && vma->vm_obj) {
        struct vm_object *obj = vma->vm_obj;
        uint64_t window = 16;
        uint64_t start_pgoff = vmf.pgoff > (window / 2) ? vmf.pgoff - (window / 2) : 0;
        if (start_pgoff < vma->vm_pgoff) start_pgoff = vma->vm_pgoff;

        uint64_t end_pgoff = vmf.pgoff + (window / 2);
        
        down_read(&obj->lock);
        for (uint64_t off = start_pgoff; off <= end_pgoff; off++) {
          if (off == vmf.pgoff) continue;
          if (off >= (obj->size >> PAGE_SHIFT)) break;
          
          struct folio *f = vm_object_find_folio(obj, off);
          if (f) {
             uint64_t addr = vma->vm_start + ((off - (vma->vm_pgoff)) << PAGE_SHIFT);
             if (addr >= vma->vm_start && addr < vma->vm_end) {
                /* Map it if not already present (vmm_map_page handles present check) */
                vmm_map_page(vma->vm_mm, addr, folio_to_phys(f), vmf.prot);
             }
          }
        }
        up_read(&obj->lock);
      }
    }

    /*
     * Handle Shared Writable Mappings & page_mkwrite.
     * If this is a write fault on a shared mapping, we must notify the owner.
     */
    if ((vma->vm_flags & VM_SHARED) && (flags & FAULT_FLAG_WRITE)) {
      int mk_ret = 0;

      if (vma->vm_ops && vma->vm_ops->page_mkwrite) {
        mk_ret = vma->vm_ops->page_mkwrite(vma, &vmf);
      } else if (vma->vm_obj && vma->vm_obj->ops && vma->vm_obj->ops->page_mkwrite) {
        mk_ret = vma->vm_obj->ops->page_mkwrite(vma->vm_obj, vma, &vmf);
      }

      if (mk_ret != 0) return mk_ret;

      /* Mark folio as dirty for writeback */
      folio->page.flags |= PG_dirty;

      /* Re-map with write permissions now that mkwrite succeeded */
      vmm_map_page(vma->vm_mm, vmf.address, phys, vmf.prot | PTE_RW);
    }
  }

  return 0;
}
