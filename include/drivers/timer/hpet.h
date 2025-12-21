#pragma once

#include <kernel/types.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>

// HPET register offsets
#define HPET_GENERAL_CAPABILITIES_ID 0x00
#define HPET_GENERAL_CONFIGURATION 0x10
#define HPET_GENERAL_INTERRUPT_STATUS 0x20
#define HPET_MAIN_COUNTER_VALUE 0xF0
#define HPET_MAIN_COUNTER_VALUE_HIGH 0xF4

// HPET Configuration Register bits
#define HPET_CONF_ENABLE_CNF (1 << 0)
#define HPET_CONF_LEGACY_REPLACEMENT_CNF (1 << 1)

// HPET capabilities register bits
#define HPET_CAP_COUNT_SIZE_CAP (1 << 13)

// HPET driver state
typedef struct {
  uint64_t base_address;
  uint64_t period_fs;   // Period in femtoseconds
  uint8_t counter_size; // 32 or 64 bit
  uint8_t num_comparators;
  uint8_t revision_id;
  uint16_t vendor_id;
  uint8_t page_protection;
  bool initialized;
  bool enabled;
} hpet_info_t;

extern hpet_info_t hpet_info;

// HPET driver functions
int hpet_init(void);
bool hpet_available(void);
uint64_t hpet_get_counter(void);
uint64_t hpet_get_time_ns(void);
void hpet_enable(void);
void hpet_disable(void);
int hpet_calibrate_tsc(void); // Recalibrate TSC using HPET

// Time Subsystem Integration
struct time_source; // forward declare
const struct time_source *hpet_get_time_source(void);