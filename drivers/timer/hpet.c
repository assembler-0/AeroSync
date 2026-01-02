/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/timer/hpet.c
 * @brief HPET timer driver and time source integration
 * @copyright (C) 2025 assembler-0
 */

#include <arch/x86_64/mm/paging.h>
#include <drivers/timer/hpet.h>
#include <kernel/classes.h> 
#include <kernel/fkx/fkx.h>
#include <lib/printk.h>
#include <arch/x86_64/tsc.h>
#include <mm/vmalloc.h>

hpet_info_t hpet_info = {0};
static void *hpet_mapped_base = NULL;

static int hpet_validate(void);

static inline uint32_t hpet_read32(uint32_t offset) {
  if (!hpet_mapped_base)
    return 0;
  volatile uint32_t *addr =
      (volatile uint32_t *)((uintptr_t)hpet_mapped_base + offset);
  return *addr;
}

static inline void hpet_write32(uint32_t offset, uint32_t value) {
  if (!hpet_mapped_base)
    return;
  volatile uint32_t *addr =
      (volatile uint32_t *)((uintptr_t)hpet_mapped_base + offset);
  *addr = value;
}

static inline uint64_t hpet_read64(uint32_t offset) {
  if (!hpet_mapped_base)
    return 0;
  if (hpet_info.counter_size == 64) {
    volatile uint64_t *addr =
        (volatile uint64_t *)((uintptr_t)hpet_mapped_base + offset);
    return *addr;
  } else {
    uint32_t low = hpet_read32(offset);
    uint32_t high = hpet_read32(offset + 4);
    return ((uint64_t)high << 32) | low;
  }
}

static inline void hpet_write64(uint32_t offset, uint64_t value) {
  if (!hpet_mapped_base)
    return;
  if (hpet_info.counter_size == 64) {
    volatile uint64_t *addr =
        (volatile uint64_t *)((uintptr_t)hpet_mapped_base + offset);
    *addr = value;
  } else {
    hpet_write32(offset, (uint32_t)value);
    hpet_write32(offset + 4, (uint32_t)(value >> 32));
  }
}

static int hpet_source_init(void) {
  if (hpet_init() != 0)
    return -1;
  return 0;
}

static uint64_t hpet_source_get_frequency(void) {
  if (hpet_info.period_fs == 0)
    return 10000000;
  return 1000000000000000ULL / hpet_info.period_fs;
}

static uint64_t hpet_source_read_counter(void) { return hpet_get_counter(); }

static int hpet_source_calibrate_tsc_impl(void) { return hpet_calibrate_tsc(); }

static time_source_t hpet_time_source = {
    .name = "HPET",
    .priority = 200,
    .type = TIME_SOURCE_HPET,
    .init = hpet_source_init,
    .get_frequency = hpet_source_get_frequency,
    .read_counter = hpet_source_read_counter,
    .calibrate_tsc = hpet_source_calibrate_tsc_impl,
};

const time_source_t *hpet_get_time_source(void) { return &hpet_time_source; }

static int hpet_validate(void) {
  if (!hpet_available()) {
    return -1;
  }

  uint64_t capabilities = hpet_read64(HPET_GENERAL_CAPABILITIES_ID);
  if (capabilities == 0 || capabilities == 0xFFFFFFFF) {
    printk(KERN_ERR HPET_CLASS "Invalid HPET capabilities: 0x%lx\n",
           capabilities);
    return -1;
  }

  uint8_t detected_counter_size =
      (capabilities & HPET_CAP_COUNT_SIZE_CAP) ? 64 : 32;
  if (detected_counter_size != hpet_info.counter_size) {
    printk(KERN_WARNING HPET_CLASS
           "Counter size mismatch: detected %d-bit, expected %d-bit\n",
           detected_counter_size, hpet_info.counter_size);
  }

  printk(HPET_CLASS "HPET validation passed\n");
  return 0;
}

int hpet_init(void) {
  if (hpet_info.initialized)
    return 0;

  uacpi_table hpet_table;
  uacpi_status status;

  printk(HPET_CLASS "Initializing HPET driver...\n");

  status = uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &hpet_table);
  if (uacpi_unlikely_error(status)) {
    printk(KERN_WARNING HPET_CLASS "HPET table not found: %s\n",
           uacpi_status_to_string(status));
    return -1;
  }

  const struct acpi_hpet *hpet = (const struct acpi_hpet *)hpet_table.hdr;

  hpet_info.base_address = hpet->address.address;
  hpet_info.revision_id = hpet->block_id & 0xFF;
  hpet_info.vendor_id =
      (hpet->block_id >> ACPI_HPET_PCI_VENDOR_ID_SHIFT) & 0xFFFF;
  hpet_info.num_comparators =
      ((hpet->block_id >> ACPI_HPET_NUMBER_OF_COMPARATORS_SHIFT) &
       ACPI_HPET_NUMBER_OF_COMPARATORS_MASK) +
      1;
  hpet_info.page_protection = hpet->flags & ACPI_HPET_PAGE_PROTECTION_MASK;

  printk(KERN_DEBUG HPET_CLASS "HPET found:\n");
  printk(KERN_DEBUG HPET_CLASS "  Base Address: 0x%lx\n", hpet_info.base_address);
  printk(KERN_DEBUG HPET_CLASS "  Revision: %d\n", hpet_info.revision_id);
  printk(KERN_DEBUG HPET_CLASS "  Vendor ID: 0x%x\n", hpet_info.vendor_id);
  printk(KERN_DEBUG HPET_CLASS "  Num Comparators: %d\n", hpet_info.num_comparators);
  printk(KERN_DEBUG HPET_CLASS "  Page Protection: %d\n", hpet_info.page_protection);

  hpet_mapped_base = viomap(hpet_info.base_address, PAGE_SIZE);
  if (!hpet_mapped_base) {
    printk(KERN_ERR HPET_CLASS "Failed to map HPET registers\n");
    uacpi_table_unref(&hpet_table);
    return -1;
  }

  uint64_t capabilities = hpet_read64(HPET_GENERAL_CAPABILITIES_ID);
  hpet_info.counter_size = (capabilities & HPET_CAP_COUNT_SIZE_CAP) ? 64 : 32;
  hpet_info.period_fs = (capabilities >> 32) & 0xFFFFFFFF;

  if (hpet_info.period_fs == 0) {
    printk(KERN_WARNING HPET_CLASS
           "HPET period is 0 (likely emulation bug), falling back to standard "
           "14.318MHz period\n");
    hpet_info.period_fs = 69841279;
  }

  printk(KERN_DEBUG HPET_CLASS "  Period: %lu fs\n", hpet_info.period_fs);

  hpet_disable();
  hpet_write64(HPET_MAIN_COUNTER_VALUE, 0);
  hpet_info.initialized = true;

  uacpi_table_unref(&hpet_table);
  hpet_enable();

  if (hpet_validate() != 0) {
    printk(KERN_ERR HPET_CLASS "HPET validation failed\n");
    return -1;
  }

  printk(HPET_CLASS "HPET driver initialized successfully\n");
  return 0;
}

bool hpet_available(void) { return hpet_info.initialized && hpet_info.enabled; }

uint64_t hpet_get_counter(void) {
  if (!hpet_available()) {
    return 0;
  }
  return hpet_read64(HPET_MAIN_COUNTER_VALUE);
}

uint64_t hpet_get_time_ns(void) {
  if (!hpet_available()) {
    return get_time_ns();
  }

  uint64_t counter = hpet_get_counter();
  return (counter * hpet_info.period_fs) / 1000000ULL;
}

void hpet_enable(void) {
  if (!hpet_info.initialized) {
    return;
  }

  uint32_t config = hpet_read32(HPET_GENERAL_CONFIGURATION);
  config |= HPET_CONF_ENABLE_CNF;
  hpet_write32(HPET_GENERAL_CONFIGURATION, config);

  hpet_info.enabled = true;
}

void hpet_disable(void) {
  if (!hpet_info.initialized) {
    return;
  }

  uint32_t config = hpet_read32(HPET_GENERAL_CONFIGURATION);
  config &= ~HPET_CONF_ENABLE_CNF;
  hpet_write32(HPET_GENERAL_CONFIGURATION, config);

  hpet_info.enabled = false;
}

int hpet_calibrate_tsc(void) {
  if (!hpet_available()) {
    printk(KERN_WARNING HPET_CLASS "HPET not available for TSC calibration\n");
    return -1;
  }

  printk(HPET_CLASS "Starting TSC recalibration using HPET...\n");

  uint64_t total_tsc_freq = 0;
  int measurements = 0;
  const int num_samples = 2;

  for (int i = 0; i < num_samples; i++) {
    uint64_t hpet_start = hpet_get_time_ns();
    uint64_t tsc_start = rdtsc();

    uint64_t tsc_start_delay = rdtsc();
    uint64_t tsc_freq = tsc_freq_get(); 
    uint64_t tsc_ticks_for_100ms = (tsc_freq * 100) / 100;

    uint64_t hpet_wait_until = hpet_start + 7000000;
    uint64_t tsc_wait_until = tsc_start_delay + tsc_ticks_for_100ms;

    while (hpet_get_time_ns() < hpet_wait_until) {
      if (tsc_freq > 0 && rdtsc() > tsc_wait_until) {
        printk(KERN_WARNING HPET_CLASS
               "Safety timeout triggered in calibration sample %d\n",
               i + 1);
        break;
      }
      cpu_relax();
    }

    uint64_t hpet_end = hpet_get_time_ns();
    uint64_t tsc_end = rdtsc();

    uint64_t hpet_elapsed_ns = hpet_end - hpet_start;
    uint64_t tsc_elapsed = tsc_end - tsc_start;

    if (hpet_elapsed_ns > 0 && hpet_elapsed_ns <= 150000000) {
      uint64_t sample_freq = (tsc_elapsed * 1000000000ULL) / hpet_elapsed_ns;
      total_tsc_freq += sample_freq;
      measurements++;

      printk(KERN_DEBUG HPET_CLASS "Sample %d: HPET elapsed: %lu ns, TSC elapsed: %lu "
                        "ticks, freq: %lu Hz\n",
             i + 1, hpet_elapsed_ns, tsc_elapsed, sample_freq);
    }
  }

  if (measurements > 0) {
    uint64_t avg_tsc_freq = total_tsc_freq / measurements;
    printk(KERN_DEBUG HPET_CLASS "Average TSC frequency from %d samples: %lu Hz\n",
           measurements, avg_tsc_freq);
    tsc_recalibrate_with_freq(avg_tsc_freq);
    printk(HPET_CLASS "TSC recalibrated using HPET reference\n");
    return 0;
  }

  return -1;
}
