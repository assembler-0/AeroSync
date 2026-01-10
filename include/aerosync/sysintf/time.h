/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/drivers/timer/time.h
 * @brief Unified Time Subsystem interface
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#pragma once

#include <compiler.h>
#include <aerosync/types.h>

typedef enum {
  TIME_SOURCE_PIT = 0,
  TIME_SOURCE_HPET = 1,
  TIME_SOURCE_TSC = 2, // Usually used as a secondary calibrated source
  TIME_SOURCE_OTHER = 0xFF
} time_source_type_t;

typedef struct time_source {
  const char *name;
  uint32_t priority; // Higher priority sources are preferred
  time_source_type_t type;

  // Initialize the hardware
  int (*init)(void);

  // Get the frequency of the counter in Hz
  uint64_t (*get_frequency)(void);

  // Read the current counter value
  uint64_t (*read_counter)(void);

  // Optional: Recalibrate TSC using this source
  // Returns 0 on success, -1 on failure.
  // Ideally, this function should take roughly 'wait_ns' nanoseconds to execute
  // and return the calculated TSC frequency.
  int (*calibrate_tsc)(void);
} time_source_t;

/**
 * @brief Register a new time source with the subsystem.
 * @param source Pointer to the time source structure.
 */
void time_register_source(const time_source_t *source);

/**
 * @brief Initialize the Time Subsystem, selecting the best available source.
 * @return 0 on success, -1 if no source could be initialized.
 */
int time_init(void);

/**
 * @brief Get the name of the currently active time source.
 * @return String containing the source name.
 */
const char *time_get_source_name(void);

/**
 * @brief Wait for a specified number of nanoseconds using the active time
 * source.
 * @param ns Nanoseconds to wait.
 */
void time_wait_ns(uint64_t ns);

/**
 * @brief Busy-wait for a specified number of microseconds.
 */
static __always_inline void delay_us(uint64_t us) { time_wait_ns(us * 1000ULL); }

/**
 * @brief Busy-wait for a specified number of milliseconds.
 */
static __always_inline void delay_ms(uint64_t ms) { time_wait_ns(ms * 1000000ULL); }

/**
 * @brief Busy-wait for a specified number of seconds.
 */
static __always_inline void delay_s(uint64_t s) { time_wait_ns(s * 1000000000ULL); }

/**
 * @brief Calibrate the TSC using the currently active time source.
 * @return 0 on success, -1 on failure.
 */
int time_calibrate_tsc_system(void);
