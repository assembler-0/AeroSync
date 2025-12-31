#pragma once

#include <mm/mm_types.h>

/**
 * @file include/mm/mmu_gather.h
 * @brief MMU gather structure for batching TLB flushes and page freeing
 */

#define MAX_GATHER_PAGES 512

struct mmu_gather {
    struct mm_struct *mm;
    uint64_t start;
    uint64_t end;
    
    // Batched pages to free
    uint64_t pages[MAX_GATHER_PAGES];
    size_t nr_pages;
    
    bool full_flush;
};

void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm, uint64_t start, uint64_t end);
void tlb_finish_mmu(struct mmu_gather *tlb);
void tlb_remove_page(struct mmu_gather *tlb, uint64_t phys, uint64_t virt);
