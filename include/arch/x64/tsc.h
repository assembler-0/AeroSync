#include <kernel/types.h>

uint64_t rdtsc();
uint64_t rdtscp();
uint64_t tsc_freq_get(const uint32_t timer_hz_before);
uint64_t get_tsc_freq(void);
uint64_t calibrate_tsc(void);
uint64_t get_time_ns();
void tsc_delay(uint64_t ns);
void tsc_delay_ms(uint64_t ms);