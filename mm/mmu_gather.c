#include <mm/mmu_gather.h>
#include <arch/x64/mm/tlb.h>
#include <arch/x64/mm/pmm.h>
#include <lib/printk.h>

void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm, uint64_t start, uint64_t end) {
    tlb->mm = mm;
    tlb->start = start;
    tlb->end = end;
    tlb->nr_pages = 0;
    tlb->full_flush = false;
}

void tlb_remove_page(struct mmu_gather *tlb, uint64_t phys, uint64_t virt) {
    (void)virt;
    if (tlb->nr_pages < MAX_GATHER_PAGES) {
        tlb->pages[tlb->nr_pages++] = phys;
    } else {
        // Overflow, flush now
        vmm_tlb_shootdown(tlb->mm, tlb->start, tlb->end);
        for (size_t i = 0; i < tlb->nr_pages; i++) {
            pmm_free_page(tlb->pages[i]);
        }
        tlb->nr_pages = 0;
        tlb->full_flush = true;
    }
}

void tlb_finish_mmu(struct mmu_gather *tlb) {
    if (tlb->nr_pages > 0 || !tlb->full_flush) {
        vmm_tlb_shootdown(tlb->mm, tlb->start, tlb->end);
    }
    
    for (size_t i = 0; i < tlb->nr_pages; i++) {
        pmm_free_page(tlb->pages[i]);
    }
    tlb->nr_pages = 0;
}
