#pragma once

#include <kernel/types.h>

#define PIT_FREQUENCY_BASE 1193182

void pit_init(void);
void pit_set_frequency(uint32_t frequency);
void pit_wait(uint32_t ms);
