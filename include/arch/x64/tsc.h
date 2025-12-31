#pragma once

#include <kernel/types.h>

uint64_t rdtsc(void);
uint64_t rdtscp(void);
uint64_t tsc_freq_get(void);
void tsc_recalibrate_with_freq(uint64_t new_freq);
uint64_t get_time_ns();
void tsc_delay(uint64_t ns);
void tsc_delay_ms(uint64_t ms);
void tsc_calibrate_early(void);