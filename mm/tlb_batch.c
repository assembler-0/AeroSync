/// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file mm/tlb_batch.c
 * @brief Batched TLB shootdown for scalability
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/slub.h>
#include <mm/vm_tuning.h>
#include <arch/x86_64/mm/tlb.h>
#include <linux/list.h>
#include <aerosync/spinlock.h>

struct tlb_batch_entry {
  struct list_head list;
  uint64_t start;
  uint64_t end;
};

/**
 * tlb_batch_flush - Flush all pending TLB invalidations
 * @mm: Target address space
 *
 * Performs a single batched TLB shootdown for all accumulated ranges.
 * This reduces IPI overhead from O(n*cores) to O(cores).
 */
void tlb_batch_flush(struct mm_struct *mm) {
  if (!mm || atomic_read(&mm->tlb_batch_count) == 0) return;

  irq_flags_t flags = spinlock_lock_irqsave(&mm->tlb_batch_lock);

  /* Coalesce ranges: find min/max bounds */
  uint64_t min_addr = ULONG_MAX;
  uint64_t max_addr = 0;

  struct tlb_batch_entry *entry, *tmp;
  list_for_each_entry_safe(entry, tmp, &mm->tlb_batch_list, list) {
    if (entry->start < min_addr) min_addr = entry->start;
    if (entry->end > max_addr) max_addr = entry->end;
    list_del(&entry->list);
    kfree(entry);
  }

  atomic_set(&mm->tlb_batch_count, 0);
  spinlock_unlock_irqrestore(&mm->tlb_batch_lock, flags);

  /* Single shootdown for entire range */
  if (min_addr < max_addr) {
    vmm_tlb_shootdown(mm, min_addr, max_addr);
  }
}


/**
 * tlb_batch_add - Add a range to the TLB batch
 * @mm: Target address space
 * @start: Start address
 * @end: End address
 *
 * Batches TLB invalidations to reduce IPI storms on many-core systems.
 * Flushes automatically when batch threshold is reached.
 */
void tlb_batch_add(struct mm_struct *mm, uint64_t start, uint64_t end) {
  if (!mm) return;

  irq_flags_t flags = spinlock_lock_irqsave(&mm->tlb_batch_lock);
  
  int count = atomic_inc_return(&mm->tlb_batch_count);
  int threshold = VM_TLB_BATCH_SIZE();
  
  /* Fast path: if batch is full, flush immediately */
  if (count >= threshold) {
    spinlock_unlock_irqrestore(&mm->tlb_batch_lock, flags);
    tlb_batch_flush(mm);
    return;
  }

  /* Add to batch list */
  struct tlb_batch_entry *entry = kmalloc(sizeof(*entry));
  if (entry) {
    entry->start = start;
    entry->end = end;
    list_add_tail(&entry->list, &mm->tlb_batch_list);
  } else {
    /* OOM: flush immediately as fallback */
    atomic_dec(&mm->tlb_batch_count);
    spinlock_unlock_irqrestore(&mm->tlb_batch_lock, flags);
    vmm_tlb_shootdown(mm, start, end);
    return;
  }

  spinlock_unlock_irqrestore(&mm->tlb_batch_lock, flags);
}