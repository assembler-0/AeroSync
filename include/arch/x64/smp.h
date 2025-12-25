#pragma once

#include <kernel/types.h>
#include <limine/limine.h>

// Initialize SMP (BSP calls this)
void smp_init(void);

// Get number of CPUs found
uint64_t smp_get_cpu_count(void);

uint64_t smp_get_id(void);
int smp_is_active();