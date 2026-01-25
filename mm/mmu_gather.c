/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/mmu_gather.c
 * @brief Efficient TLB shootdown
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

#include <mm/mmu_gather.h>
#include <arch/x86_64/mm/tlb.h>
#include <arch/x86_64/mm/pmm.h>
#include <mm/page.h>

void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm, uint64_t start, uint64_t end) {
    tlb->mm = mm;
    tlb->start = start;
    tlb->end = end;
    tlb->nr_folios = 0;
    tlb->full_flush = false;
}

void tlb_remove_folio(struct mmu_gather *tlb, struct folio *folio, uint64_t virt) {
    (void)virt;
    if (tlb->nr_folios >= MAX_GATHER_PAGES) {
        // Overflow, flush now
        vmm_tlb_shootdown(tlb->mm, tlb->start, tlb->end);
        for (size_t i = 0; i < tlb->nr_folios; i++) {
            folio_put(tlb->folios[i]);
        }
        tlb->nr_folios = 0;
        tlb->full_flush = true;
    }
    tlb->folios[tlb->nr_folios++] = folio;
}

void tlb_finish_mmu(struct mmu_gather *tlb) {
    if (tlb->nr_folios > 0 || !tlb->full_flush) {
        vmm_tlb_shootdown(tlb->mm, tlb->start, tlb->end);
    }
    
    for (size_t i = 0; i < tlb->nr_folios; i++) {
        folio_put(tlb->folios[i]);
    }
    tlb->nr_folios = 0;
}
