#pragma once

#include <aerosync/types.h>

#define PIT_FREQUENCY_BASE 1193182
#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CH1_PORT 0x41
#define PIT_CH2_PORT 0x42

void pit_init(void);
void pit_set_frequency(uint32_t frequency);
void pit_wait(uint32_t ms);
void pit_calibrate_tsc(void);

// Time Subsystem Integration
struct time_source; // forward declare
const struct time_source *pit_get_time_source(void);
