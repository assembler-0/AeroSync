///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/topology.c
 * @brief CPU topology detection (SMT, Core, Die, Package)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

DEFINE_PER_CPU(struct cpuinfo_x86, cpu_info);

/**
 * detect_core_type - Use CPUID to detect hybrid core type
 */
static void detect_core_type(struct cpuinfo_x86 *ci) {
    uint32_t eax, ebx, ecx, edx;
    
    /* 1. Check if Hybrid is supported (CPUID.07H.0:EDX[15]) */
    cpuid_count(0x07, 0, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1 << 15))) {
        ci->core_type = CORE_TYPE_UNKNOWN;
        return;
    }

    /* 2. Get core type from CPUID.1AH.0:EAX[31:24] */
    /* Leaf 0x1A provides Native Model ID and Core Type */
    cpuid(0x1A, &eax, &ebx, &ecx, &edx);
    ci->core_type = (enum x86_core_type)((eax >> 24) & 0xFF);
}

/**
 * detect_topology_leaf_0b - Parse CPUID Leaf 0x0B (Extended Topology)
 *
 * This leaf provides hierarchical ID information (Thread -> Core -> Package)
 */
static void detect_topology_leaf_0b(struct cpuinfo_x86 *ci) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t initial_apic_id;
    
    /* Get initial APIC ID from leaf 0x0B or 0x01 */
    cpuid(0x01, &eax, &ebx, &ecx, &edx);
    initial_apic_id = (ebx >> 24) & 0xFF;

    /* Iterate through topology levels */
    for (int level = 0; ; level++) {
        cpuid_count(0x0B, level, &eax, &ebx, &ecx, &edx);
        
        /* Level type 0 means no more levels */
        uint32_t level_type = (ecx >> 8) & 0xFF;
        if (level_type == 0) break;

        uint32_t shift = eax & 0x1F;
        
        switch (level_type) {
            case 1: /* SMT */
                ci->thread_id = edx & ((1 << shift) - 1);
                break;
            case 2: /* Core */
                ci->core_id = (edx >> shift) & 0xFF; // Simple heuristic
                break;
        }
    }
    
    /* edx contains the 32-bit x2APIC ID */
    /* package_id is usually the high bits */
    ci->package_id = edx >> 8; // Simplified for now
}

void detect_cpu_topology(void) {
    struct cpuinfo_x86 *ci = this_cpu_ptr(cpu_info);
    
    /* Reset */
    ci->package_id = 0;
    ci->die_id = 0;
    ci->core_id = 0;
    ci->thread_id = 0;
    ci->core_type = CORE_TYPE_UNKNOWN;

    /* Detect topology using Leaf 0x0B (Intel) */
    detect_topology_leaf_0b(ci);
    
    /* Detect core type (Intel Hybrid) */
    detect_core_type(ci);

    printk(KERN_DEBUG SMP_CLASS "CPU %d: Pkg %d Core %d Thread %d Type %s\n",
           (int)smp_get_id(), ci->package_id, ci->core_id, ci->thread_id,
           ci->core_type == CORE_TYPE_INTEL_CORE ? "P-Core" :
           ci->core_type == CORE_TYPE_INTEL_ATOM ? "E-Core" : "Standard");
}
