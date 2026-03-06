/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/tiered.c
 * @brief Tiered Memory Management (HBM/DRAM Migration)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sysintf/time.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/printk.h>
#include <mm/mm_types.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/zone.h>


#define TIER_SCAN_INTERVAL_MS 5000
#define PAGE_HOT_THRESHOLD 2
#define PAGE_COLD_THRESHOLD 0

/* Memory Tiers (Node classification) */
#define TIER_HBM  0  /* Fast, low-capacity */
#define TIER_DRAM 1  /* Standard */

static int node_to_tier(int nid) {
  /* 
   * Heuristic: Nodes with distance 0 to self but very high bandwidth
   * In a real system, we'd check ACPI HMAT. 
   * Here we assume Node 0 is HBM if it's small and Node 1 is DRAM.
   */
  if (nid == 0) return TIER_HBM;
  return TIER_DRAM;
}

static int ktiered_thread(void *unused) {
  (void) unused;
  printk(KERN_INFO VMM_CLASS "ktiered started (Tiered Memory Engine)\n");

  while (1) {
    delay_ms(TIER_SCAN_INTERVAL_MS);

    /*
     * SCANNING PHASE: Iterate through all processes and VMAs.
     * We look for 'hot' pages in DRAM to promote and 'cold' pages in HBM to demote.
     */
    struct task_struct *task;
    irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
    list_for_each_entry(task, &task_list, tasks) {
      struct mm_struct *mm = task->mm;
      if (!mm) continue;

      if (!down_read_trylock(&mm->mmap_lock))
        continue;

      struct vm_area_struct *vma;
      for_each_vma(mm, vma) {
        if (!vma->vm_obj || (vma->vm_flags & (VM_IO | VM_PFNMAP)))
          continue;

        struct vm_object *obj = vma->vm_obj;
        /* We only care about anonymous memory for now */
        if (!(obj->type == VM_OBJECT_ANON)) continue;

        unsigned long index;
        struct folio *folio;
        xa_for_each(&obj->page_tree, index, folio) {
          if (xa_is_internal(folio)) continue;

          uint64_t virt = vma->vm_start + ((index - vma->vm_pgoff) << PAGE_SHIFT);
          if (virt < vma->vm_start || virt >= vma->vm_end) continue;

          int nid = folio->node;
          int tier = node_to_tier(nid);

          /* Check heat via Accessed bit */
          bool accessed = vmm_is_accessed(mm, virt);
          if (accessed) {
            vmm_clear_accessed(mm, virt);
            /* Increment heat - we could store this in folio->flags or a separate map */
            // For now, let's just use immediate promotion if hit in Tier 1
            if (tier == TIER_DRAM) {
              /* PROMOTION: Move to HBM (Node 0) */
              // printk(KERN_DEBUG "ktiered: Promoting folio at %llx to HBM", virt);
              migrate_folio_to_node(folio, 0);
            }
          } else {
            /* Page is cold */
            if (tier == TIER_HBM) {
              /* DEMOTION: Move to DRAM (Node 1) to free up fast HBM */
              // printk(KERN_DEBUG "ktiered: Demoting folio at %llx to DRAM", virt);
              migrate_folio_to_node(folio, 1);
            }
          }
        }
      }
      up_read(&mm->mmap_lock);
    }
    spinlock_unlock_irqrestore(&tasklist_lock, flags);
  }
  return 0;
}

int ktiered_init(void) {
  struct task_struct *k = kthread_create(ktiered_thread, nullptr, "ktiered");
  if (!k) return -ENOMEM;
  kthread_run(k);
  return 0;
}
