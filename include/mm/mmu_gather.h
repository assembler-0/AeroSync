#pragma once

#include <mm/mm_types.h>

/**
 * @file include/mm/mmu_gather.h
 * @brief MMU gather structure for batching TLB flushes and page freeing
 */

#define MAX_GATHER_PAGES 128

struct mmu_gather {
    struct mm_struct *mm;
    uint64_t start;
    uint64_t end;
    
    // Batched folios to free
    struct folio *folios[MAX_GATHER_PAGES];
    size_t nr_folios;
    
    bool full_flush;
};

void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm, uint64_t start, uint64_t end);
void tlb_finish_mmu(struct mmu_gather *tlb);
void tlb_remove_folio(struct mmu_gather *tlb, struct folio *folio, uint64_t virt);

/* Legacy helper */
static inline void tlb_remove_page(struct mmu_gather *tlb, uint64_t phys, uint64_t virt) {
    (void)phys;
    (void)tlb;
    (void)virt;
    /* This should be migrated to tlb_remove_folio */
}
