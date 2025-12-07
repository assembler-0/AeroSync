#pragma once

#include <kernel/types.h>
#include <limine/limine.h>

// Initialize SMP (BSP calls this)
void smp_init(void);

// Get number of CPUs found
uint64_t smp_get_cpu_count(void);

// Get current CPU ID (LAPIC ID)
// Note: Requires parsing MADT/APIC which we might not have fully done yet,
// but Limine gives us an ID. For now, we use Limine's ID.
uint64_t smp_get_id(void);
