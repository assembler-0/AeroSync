/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/khugepaged.c
 * @brief Transparent Huge Page (THP) daemon
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

#include <mm/vma.h>
#include <mm/mm_types.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <linux/container_of.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <arch/x86_64/tsc.h>
#include <mm/vm_object.h>

#define SCAN_BATCH_VMAS 16

static void khugepaged_scan_mm(struct mm_struct *mm) {
  struct vm_area_struct *vma;
  int scanned = 0;

  /* 
   * Use RCU walk for scanning to minimize mmap_lock contention.
   * We only take mmap_lock if we find a candidate for collapsing.
   */
  rcu_read_lock();
  uint32_t mm_seq = atomic_read(&mm->mmap_seq);

  for_each_vma(mm, vma) {
    if (scanned++ >= SCAN_BATCH_VMAS) break;

    /* Only anonymous VMAs that allow huge pages */
    if (!vma->vm_obj || vma->vm_obj->type != VM_OBJECT_ANON) continue;
    
    /* 
     * Skip if layout changed since we started.
     * We'll try again in the next pass.
     */
    if (atomic_read(&mm->mmap_seq) != mm_seq) break;

    if (vma->vm_flags & (VM_NOHUGEPAGE | VM_IO | VM_PFNMAP)) continue;

    uint32_t vma_seq = __atomic_load_n(&vma->vma_seq, __ATOMIC_ACQUIRE);

    /* Align range to 2MB */
    uint64_t start = (vma->vm_start + 0x1FFFFFULL) & 0xFFFFFFFFFFE00000ULL;
    uint64_t end = vma->vm_end & 0xFFFFFFFFFFE00000ULL;

    for (uint64_t addr = start; addr + 0x200000 <= end; addr += 0x200000) {
      /* Re-verify VMA state before attempting heavy collapse */
      if (atomic_read(&mm->mmap_seq) != mm_seq || vma->vma_seq != vma_seq) break;

      /* 
       * Attempt to merge. vmm_merge_to_huge will verify if
       * all 512 pages are present and contiguous.
       * We still need mmap_lock for the actual merge because it modifies page tables.
       */
      rcu_read_unlock();
      if (down_read_trylock(&mm->mmap_lock)) {
          if (atomic_read(&mm->mmap_seq) == mm_seq && vma->vma_seq == vma_seq) {
              if (vmm_merge_to_huge(mm, addr, VMM_PAGE_SIZE_2M) == 0) {
                  // printk(KERN_DEBUG THP_CLASS "collapsed 2MB huge page at %llx\n", addr);
              }
          }
          up_read(&mm->mmap_lock);
      }
      rcu_read_lock();
    }
  }
  rcu_read_unlock();
}

static int khugepaged_thread(void *unused) {
  (void) unused;
  printk(KERN_INFO THP_CLASS "khugepaged started\n");

  while (1) {
    struct mm_struct *mms[64];
    int mm_count = 0;

    /* Collect MMs under lock to avoid iterating task_list while unlocked */
    irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
    struct task_struct *p;
    list_for_each_entry(p, &task_list, tasks) {
      if (p->mm && p->mm != &init_mm) {
        // Avoid duplicates if multiple threads share the same MM
        bool duplicate = false;
        for (int i = 0; i < mm_count; i++) {
          if (mms[i] == p->mm) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) continue;

        mm_get(p->mm);
        mms[mm_count++] = p->mm;
        if (mm_count >= 64) break;
      }
    }
    spinlock_unlock_irqrestore(&tasklist_lock, flags);

    /* Scan collected MMs */
    for (int i = 0; i < mm_count; i++) {
      khugepaged_scan_mm(mms[i]);
      mm_put(mms[i]);
    }

    /* Occasionally scan init_mm (kernel vmalloc space) */
    static int init_mm_scan_count = 0;
    if (init_mm_scan_count++ % 10 == 0) {
      khugepaged_scan_mm(&init_mm);
    }

    /* Throttle the thread to avoid CPU starvation and excessive scanning */
    tsc_delay_ms(100);
  }
  return 0;
}

int khugepaged_init(void) {
  struct task_struct *k = kthread_create(khugepaged_thread, nullptr, "khugepaged");
  if (!k)
    return -ENOMEM;
  kthread_run(k);
  return 0;
}
