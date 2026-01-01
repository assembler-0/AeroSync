#include <arch/x64/mm/tlb.h>
#include <arch/x64/cpu.h>
#include <arch/x64/features/features.h>
#include <kernel/sysintf/ic.h>
#include <kernel/sched/sched.h>
#include <lib/printk.h>

#include <arch/x64/smp.h>

/**
 * TLB Shootdown implementation for VoidFrameX (PCID-Aware)
 */

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

static void tlb_ipi_handler(cpu_regs *regs) {
    (void)regs;
    vmm_tlb_flush_all_local();
}

void vmm_tlb_shootdown(struct mm_struct *mm, uint64_t start, uint64_t end) {
    (void)start;
    (void)end;

    // 1. Flush local TLB
    vmm_tlb_flush_all_local();

    // 2. Send IPI only if SMP is active and we have other CPUs
    if (smp_is_active()) {
        ic_send_ipi(0xFF, TLB_FLUSH_IPI_VECTOR, 0 /* Fixed */);
    }
}

void vmm_tlb_init(void) {
    // Registered via irq_install_handler in irq.c or here
    // But TLB_FLUSH_IPI_VECTOR needs to be handled in irq_common_stub if not standard IRQ
}
