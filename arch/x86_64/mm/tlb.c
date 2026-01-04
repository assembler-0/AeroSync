/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/mm/tlb.c
 * @brief TLB management for the x86_64 architecture (PCID aware)
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <arch/x86_64/mm/tlb.h>
#include <arch/x86_64/features/features.h>
#include <mm/mm_types.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/mm/paging.h>
#include <mm/vma.h>

struct invpcid_desc {
    uint64_t pcid : 12;
    uint64_t rsvd : 52;
    uint64_t addr;
};

static inline void __invpcid(uint64_t type, uint16_t pcid, uint64_t addr) {
    struct invpcid_desc desc = {pcid, 0, addr};
    __asm__ volatile("invpcid %1, %0" : : "r"(type), "m"(desc) : "memory");
}

void vmm_tlb_flush_local(uint64_t addr) {
    // invlpg is sufficient for the current PCID and for Global pages
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

void vmm_tlb_flush_all_local(void) {
    cpu_features_t *features = get_cpu_features();
    if (features->pcid) {
        if (features->invpcid) {
            __invpcid(2, 0, 0);
            return;
        }
    }

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

struct tlb_shootdown_info {
    uint64_t start;
    uint64_t end;
    bool full_flush;
};

static void tlb_shootdown_callback(void *info) {
    struct tlb_shootdown_info *si = info;
    
    if (si->full_flush) {
        vmm_tlb_flush_all_local();
    } else {
        for (uint64_t addr = si->start; addr < si->end; addr += PAGE_SIZE) {
            vmm_tlb_flush_local(addr);
        }
    }
}

void tlb_ipi_handler(void *regs) {
    (void)regs;
    // This is the old vector handler, we can leave it for now or remove if unused.
}

void vmm_tlb_shootdown(struct mm_struct *mm, uint64_t start, uint64_t end) {
    struct tlb_shootdown_info info;
    info.start = start;
    info.end = end;
    info.full_flush = (end - start > 0x10000);

    /* Memory barrier to ensure page table updates are visible before TLB flush */
    __asm__ volatile("mfence" ::: "memory");

    // 1. Flush local TLB
    tlb_shootdown_callback(&info);

    // 2. Send IPI only if SMP is active
    if (smp_is_active()) {
        if (!mm || mm == &init_mm) {
            // Global shootdown (kernel space) - target all online CPUs
            smp_call_function(tlb_shootdown_callback, &info, true);
        } else {
            // Target only CPUs using this mm
            smp_call_function_many(&mm->cpu_mask, tlb_shootdown_callback, &info, true);
        }
    }
    
    /* Memory barrier to ensure TLB flushes complete before returning */
    __asm__ volatile("mfence" ::: "memory");
}

void vmm_tlb_init(void) {
    // Registered via irq_install_handler in irq.c or here
    // But TLB_FLUSH_IPI_VECTOR needs to be handled in irq_common_stub if not standard IRQ
}
