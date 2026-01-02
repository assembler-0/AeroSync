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
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/features/features.h>
#include <kernel/sysintf/ic.h>
#include <kernel/sched/sched.h>
#include <mm/mm_types.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/mm/paging.h>
#include <mm/vma.h>
#include <arch/x86_64/cpu.h>

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
        // If PCID is enabled, a simple CR3 reload only flushes the current PCID.
        // We might want to flush all PCIDs if this is a major change.
        if (features->invpcid) {
            // Type 2: All PCIDs except global
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

static struct tlb_shootdown_info global_shootdown_info;
static spinlock_t shootdown_lock = 0;
static atomic_t shootdown_wait_count = {0};

void tlb_ipi_handler(void *regs) {
    (void)regs;
    
    if (global_shootdown_info.full_flush) {
        vmm_tlb_flush_all_local();
    } else {
        for (uint64_t addr = global_shootdown_info.start; 
             addr < global_shootdown_info.end; 
             addr += PAGE_SIZE) {
            vmm_tlb_flush_local(addr);
        }
    }

    atomic_dec(&shootdown_wait_count);
}

void vmm_tlb_shootdown(struct mm_struct *mm, uint64_t start, uint64_t end) {
    // 1. Flush local TLB
    if (end - start > 0x10000) { // If > 64KB, just flush all
        vmm_tlb_flush_all_local();
    } else {
        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
            vmm_tlb_flush_local(addr);
        }
    }

    // 2. Send IPI only if SMP is active and we have other CPUs
    if (smp_is_active()) {
        irq_flags_t flags = spinlock_lock_irqsave(&shootdown_lock);
        
        global_shootdown_info.start = start;
        global_shootdown_info.end = end;
        global_shootdown_info.full_flush = (end - start > 0x10000);

        int this_cpu = cpu_id();
        int target_cpus = 0;
        
        if (mm && mm != &init_mm) {
            // Target only CPUs using this mm
            for (int i = 0; i < MAX_CPUS; i++) {
                if (i == this_cpu) continue;
                if (cpumask_test_cpu(i, &mm->cpu_mask)) {
                    target_cpus++;
                }
            }
            
            if (target_cpus > 0) {
                atomic_set(&shootdown_wait_count, target_cpus);
                for (int i = 0; i < MAX_CPUS; i++) {
                    if (i == this_cpu) continue;
                    if (cpumask_test_cpu(i, &mm->cpu_mask)) {
                        ic_send_ipi(*per_cpu_ptr(cpu_apic_id, i), TLB_FLUSH_IPI_VECTOR, 0);
                    }
                }
            }
        } else {
            // Global shootdown (kernel space) - target all online CPUs
            for (int i = 0; i < MAX_CPUS; i++) {
                if (i == this_cpu) continue;
                if (*per_cpu_ptr(cpu_apic_id, i) != 0xFF) {
                    target_cpus++;
                }
            }
            
            if (target_cpus > 0) {
                atomic_set(&shootdown_wait_count, target_cpus);
                for (int i = 0; i < MAX_CPUS; i++) {
                    if (i == this_cpu) continue;
                    if (*per_cpu_ptr(cpu_apic_id, i) != 0xFF) {
                         ic_send_ipi(*per_cpu_ptr(cpu_apic_id, i), TLB_FLUSH_IPI_VECTOR, 0);
                    }
                }
            }
        }

        // Wait for all CPUs to acknowledge
        while (atomic_read(&shootdown_wait_count) > 0) {
            cpu_relax();
        }
        
        spinlock_unlock_irqrestore(&shootdown_lock, flags);
    }
}

void vmm_tlb_init(void) {
    // Registered via irq_install_handler in irq.c or here
    // But TLB_FLUSH_IPI_VECTOR needs to be handled in irq_common_stub if not standard IRQ
}
