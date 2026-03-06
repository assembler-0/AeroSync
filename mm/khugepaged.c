/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/khugepaged.c
 * @brief Transparent Huge Page (THP) daemon
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sched/process.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/printk.h>
#include <linux/container_of.h>
#include <mm/mm_types.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <mm/vma.h>
#include <linux/xarray.h>


#define SCAN_BATCH_VMAS 16
#define THP_COLLAPSE_THRESHOLD 448 /* 87.5% of pages must be present */

/**
 * collapse_and_promote_huge - Atomically replace 512 base pages with 1 huge page.
 * 
 * This function synchronizes both the hardware (VMM) and software (vm_object).
 */
static int collapse_and_promote_huge(struct mm_struct *mm, struct vm_area_struct *vma, uint64_t addr) {
  struct vm_object *obj = vma->vm_obj;
  uint64_t pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
  if (vma->vm_pgoff) pgoff += vma->vm_pgoff;

  /* 1. Allocate a 2MB huge physical page (order 9) */
  struct folio *huge_folio = alloc_pages(GFP_KERNEL, 9);
  if (!huge_folio) return -ENOMEM;

  void *huge_virt = folio_address(huge_folio);
  uint64_t *old_folios = kmalloc(512 * sizeof(void *));
  if (!old_folios) {
    folio_put(huge_folio);
    return -ENOMEM;
  }

  /* 2. Lock the object to freeze its state */
  down_write(&obj->lock);

  /* 3. Copy data from 512 base pages */
  for (int i = 0; i < 512; i++) {
    void *entry = xa_load(&obj->page_tree, pgoff + i);
    if (entry && !xa_is_internal(entry)) {
      struct folio *old_f = (struct folio *) entry;
      memcpy((char *) huge_virt + ((uint64_t) i << PAGE_SHIFT), folio_address(old_f), PAGE_SIZE);
      old_folios[i] = (uint64_t) old_f;
    } else {
      /* Missing page in range, zero it in the huge page */
      memset((char *) huge_virt + ((uint64_t) i << PAGE_SHIFT), 0, PAGE_SIZE);
      old_folios[i] = 0;
    }
  }

  /* 4. Update the XArray: replace 512 entries with 1 multi-index entry */
  /* We use xa_store_range to replace the entire 2MB span */
  xa_store_range(&obj->page_tree, pgoff, pgoff + 511, huge_folio, GFP_ATOMIC);

  /* 5. Update VMM (Hardware) */
  int ret = vmm_map_huge_page(mm, addr, folio_to_phys(huge_folio),
                              vma->vm_page_prot, VMM_PAGE_SIZE_2M);

  if (ret < 0) {
    /* Rollback - complicated in a real kernel, simplified here */
    up_write(&obj->lock);
    kfree(old_folios);
    folio_put(huge_folio);
    return ret;
  }

  /* 6. Update huge folio metadata */
  huge_folio->mapping = (void *) obj;
  huge_folio->index = pgoff;
  SetPageHead(&huge_folio->page);

  up_write(&obj->lock);

  /* 7. Cleanup old pages */
  for (int i = 0; i < 512; i++) {
    if (old_folios[i]) {
      struct folio *old_f = (struct folio *) old_folios[i];
      /* Unmap from VMM if not already handled by vmm_map_huge_page */
      // vmm_unmap_page(mm, addr + (i << PAGE_SHIFT));
      folio_put(old_f);
    }
  }

  kfree(old_folios);
  return 0;
}

static int khugepaged_scan_promotion_candidate(struct mm_struct *mm,
                                               uint64_t addr) {
  int present = 0;
  for (int i = 0; i < 512; i++) {
    if (vmm_virt_to_phys(mm, addr + ((uint64_t) i << PAGE_SHIFT))) {
      present++;
    }
  }
  return present;
}

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
    if (scanned++ >= SCAN_BATCH_VMAS)
      break;

    /* Only anonymous VMAs that allow huge pages */
    if (!vma->vm_obj || vma->vm_obj->type != VM_OBJECT_ANON)
      continue;

    /*
     * Skip if layout changed since we started.
     * We'll try again in the next pass.
     */
    if (atomic_read(&mm->mmap_seq) != mm_seq)
      break;

    if (vma->vm_flags & (VM_NOHUGEPAGE | VM_IO | VM_PFNMAP))
      continue;

    uint32_t vma_seq = __atomic_load_n(&vma->vma_seq, __ATOMIC_ACQUIRE);

    /* Align range to 2MB */
    uint64_t start = (vma->vm_start + 0x1FFFFFULL) & 0xFFFFFFFFFFE00000ULL;
    uint64_t end = vma->vm_end & 0xFFFFFFFFFFE00000ULL;

    for (uint64_t addr = start; addr + 0x200000 <= end; addr += 0x200000) {
      /* Re-verify VMA state before attempting heavy collapse */
      if (atomic_read(&mm->mmap_seq) != mm_seq || vma->vma_seq != vma_seq)
        break;

      /*
       * First, attempt a zero-copy merge if pages are already contiguous.
       */
      rcu_read_unlock();
      bool processed = false;
      if (down_read_trylock(&mm->mmap_lock)) {
        if (atomic_read(&mm->mmap_seq) == mm_seq && vma->vma_seq == vma_seq) {
          if (vmm_merge_to_huge(mm, addr, VMM_PAGE_SIZE_2M) == 0) {
            processed = true;
          } else {
            /*
             * THP AUTO-PROMOTION:
             * If they weren't contiguous, check if enough pages are present
             * to justify a physical migration to a new huge page.
             */
            int present = khugepaged_scan_promotion_candidate(mm, addr);
            if (present >= THP_COLLAPSE_THRESHOLD) {
              if (collapse_and_promote_huge(mm, vma, addr) == 0) {
                processed = true;
                printk(KERN_DEBUG THP_CLASS
                       "Auto-promoted %d pages to 2MB huge page at %llx\n",
                       present, addr);
              }
            }
          }
        }
        up_read(&mm->mmap_lock);
      }
      rcu_read_lock();
      if (processed) {
        /* Once we collapse a range, it's safer to move to next VMA or wait */
        break;
      }
    }
  }
  rcu_read_unlock();
}

static int khugepaged_thread(void *unused) {
  (void) unused;
  printk(KERN_INFO THP_CLASS
    "khugepaged started with Auto-Promotion enabled\n");

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
        if (duplicate)
          continue;

        mm_get(p->mm);
        mms[mm_count++] = p->mm;
        if (mm_count >= 64)
          break;
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
    delay_ms(100);
  }
  return 0;
}

int khugepaged_init(void) {
  struct task_struct *k =
      kthread_create(khugepaged_thread, nullptr, "khugepaged");
  if (!k)
    return -ENOMEM;
  kthread_run(k);
  return 0;
}
