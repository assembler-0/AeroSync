/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/khugepaged.c
 * @brief Transparent Huge Page (THP) background daemon
 * @copyright (C) 2025 assembler-0
 */

#include <mm/vma.h>
#include <mm/mm_types.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <linux/container_of.h>
#include <lib/printk.h>
#include <kernel/classes.h>

#define KHUGEPAGED_SLEEP_MS 10000
#define SCAN_BATCH_VMAS 16

static DECLARE_WAIT_QUEUE_HEAD(khugepaged_wait);

static int khugepaged_should_run(void *unused) {
    (void)unused;
    return 0; // Only wake up on timeout or signal
}

static void khugepaged_scan_mm(struct mm_struct *mm) {
    struct vm_area_struct *vma;
    int scanned = 0;

    down_read(&mm->mmap_lock);
    for_each_vma(mm, vma) {
        if (scanned++ >= SCAN_BATCH_VMAS) break;

        /* Only anonymous VMAs that allow huge pages */
        if (vma->vm_ops != &anon_vm_ops) continue;
        if (vma->vm_flags & VM_NOHUGEPAGE) continue;

        /* Align range to 2MB */
        uint64_t start = (vma->vm_start + 0x1FFFFFULL) & 0xFFFFFFFFFFE00000ULL;
        uint64_t end = vma->vm_end & 0xFFFFFFFFFFE00000ULL;

        for (uint64_t addr = start; addr + 0x200000 <= end; addr += 0x200000) {
            /* 
             * Attempt to merge. vmm_merge_to_huge will verify if 
             * all 512 pages are present and contiguous.
             */
            /* 
             * Attempt to merge. vmm_merge_to_huge will verify if 
             * all 512 pages are present and contiguous.
             */
            if (vmm_merge_to_huge(mm, addr, VMM_PAGE_SIZE_2M) == 0) {
                // printk(KERN_DEBUG "[THP] Collapsed 2MB huge page at %llx\n", addr);
            }
        }
    }
    up_read(&mm->mmap_lock);
}

static int khugepaged_thread(void *unused) {
    (void)unused;
    printk(KERN_INFO THP_CLASS "khugepaged started\n");

    while (1) {
        /* Sleep between scans */
        wait_event_timeout(khugepaged_wait, khugepaged_should_run, NULL, KHUGEPAGED_SLEEP_MS);

        /* Scan all MMs */
        irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
        struct task_struct *p;
        list_for_each_entry(p, &task_list, tasks) {
            struct mm_struct *mm = p->mm;
            if (mm && mm != &init_mm) {
                mm_get(mm);
                spinlock_unlock_irqrestore(&tasklist_lock, flags);
                
                khugepaged_scan_mm(mm);
                
                flags = spinlock_lock_irqsave(&tasklist_lock);
                mm_put(mm);
            }
        }
        spinlock_unlock_irqrestore(&tasklist_lock, flags);
        
        /* Also scan init_mm (kernel vmalloc space) */
        khugepaged_scan_mm(&init_mm);
    }
    return 0;
}

void khugepaged_init(void) {
    struct task_struct *k = kthread_create(khugepaged_thread, NULL, "khugepaged");
    if (k) kthread_run(k);
}
