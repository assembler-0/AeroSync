#pragma once

#include <mm/mm_types.h>

/**
 * Batched TLB Shootdown API
 * Reduces IPI storms on many-core systems by coalescing invalidations.
 */

void tlb_batch_add(struct mm_struct *mm, uint64_t start, uint64_t end);
void tlb_batch_flush(struct mm_struct *mm);
