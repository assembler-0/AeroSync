#pragma once

#include <aerosync/types.h>
#include <mm/mm_types.h>

/**
 * @file include/arch/x86_64/mm/tlb.h
 * @brief TLB shootdown and management for x86_64
 */

#define TLB_FLUSH_IPI_VECTOR 0xFD

void vmm_tlb_flush_local(uint64_t addr);
void vmm_tlb_flush_all_local(void);
void vmm_tlb_shootdown(struct mm_struct *mm, uint64_t start, uint64_t end);
void tlb_ipi_handler(void *regs);

void vmm_tlb_init(void);
