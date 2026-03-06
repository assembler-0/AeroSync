/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/mm/tlb.c
 * @brief TLB management for the x86_64 architecture (PCID aware)
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

#include <arch/x86_64/features/features.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/tlb.h>
#include <arch/x86_64/smp.h>
#include <mm/mm_types.h>
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
  cpu_invlpg(addr);
}

void vmm_tlb_flush_all_local(void) {
  cpu_features_t *features = get_cpu_features();
  if (features->pcid && features->invpcid) {
    /* Type 2: Flush all contexts including globals */
    __invpcid(2, 0, 0);
    return;
  }

  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  /*
   * If Global Page Enable (PGE) is active, toggling it flushes
   * ALL TLB entries, including global ones.
   */
  if (cr4 & (1ULL << 7)) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
  } else {
    /* Fallback: Standard CR3 reload (non-global only) */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  }
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
  // This is the old vector handler, we can leave it for now or remove if
  // unused.
}

void vmm_tlb_shootdown(struct mm_struct *mm, uint64_t start, uint64_t end) {
  struct tlb_shootdown_info info;
  info.start = start & PAGE_MASK;
  info.end = PAGE_ALIGN_UP(end);

  /*
   * Full flush threshold:
   * If we are flushing more than 32 pages, a full TLB flush (CR3 reload)
   * is usually faster than 32+ invlpg instructions + context overhead.
   */
  info.full_flush = (info.end - info.start >= 32 * PAGE_SIZE);

  /* Memory barrier to ensure page table updates are visible before TLB flush */
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // 1. Flush local TLB first to minimize the window where this CPU sees old
  // data
  tlb_shootdown_callback(&info);

  // 2. Send IPI only if SMP is active and there are other CPUs to notify
  if (smp_is_active() && smp_get_cpu_count() > 1) {
    if (!mm || mm == &init_mm) {
      // Global shootdown (kernel space) - target all online CPUs
      smp_call_function(tlb_shootdown_callback, &info, true);
    } else {
      /*
       * PCID-Based Lazy Invalidation:
       * 1. CPUs in mm->cpu_mask are those that have valid PCID entries.
       * 2. We only send IPIs to CPUs currently running this mm.
       * 3. For other CPUs in cpu_mask, we increment tlb_gen to force a flush on next switch.
       */
      int current_cpu = smp_get_id();
      
      /*
       * For now, we increment tlb_gen for ALL CPUs in the mask.
       * Active CPUs will get the IPI and flush immediately.
       * Idle CPUs will check tlb_gen on their next context switch to this mm.
       */
      for (int i = 0; i < smp_get_cpu_count(); i++) {
        if (cpumask_test_cpu(i, &mm->cpu_mask)) {
          __atomic_add_fetch(&mm->tlb_gen[i], 1, __ATOMIC_RELAXED);
        }
      }

      /*
       * Send IPI only to active CPUs.
       * In AeroSync, mm->cpu_mask CURRENTLY tracks active CPUs only (as seen in switch_mm).
       * To implement full lazy invalidation, we should keep CPUs in cpu_mask even when idle,
       * but for this step, we keep the existing IPI logic which is already selective.
       */
      if (cpumask_weight(&mm->cpu_mask) > 1 ||
          !cpumask_test_cpu(current_cpu, &mm->cpu_mask)) {
        smp_call_function_many(&mm->cpu_mask, tlb_shootdown_callback, &info,
                               true);
      }
    }
  }

  /* Memory barrier to ensure TLB flushes complete before returning */
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void vmm_tlb_init(void) {
  // Registered via irq_install_handler in irq.c or here,
  // But TLB_FLUSH_IPI_VECTOR needs to be handled in irq_common_stub if not
  // standard IRQ
}
