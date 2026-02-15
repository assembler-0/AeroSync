/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/timer/pit.c
 * @brief PIT driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include <drivers/timer/pit.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/fkx/fkx.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/tsc.h>
#include <aerosync/sysintf/ic.h>
#include <arch/x86_64/cpu.h>

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CH1_PORT 0x41
#define PIT_CH2_PORT 0x42

static uint32_t global_pit_frequency = IC_DEFAULT_TICK; 
static uint16_t pit_reload_value = 0;

void pit_set_frequency(uint32_t frequency) {
  if (frequency == 0)
    frequency = 100;
  if (frequency > PIT_FREQUENCY_BASE)
    frequency = PIT_FREQUENCY_BASE;

  global_pit_frequency = frequency;

  uint32_t divisor = PIT_FREQUENCY_BASE / frequency;
  if (divisor > 65535)
    divisor = 65535;

  pit_reload_value = (uint16_t)divisor;

  irq_flags_t flags = save_irq_flags();

  outb(PIT_CMD_PORT, 0x34);
  io_wait();
  outb(PIT_CH0_PORT, divisor & 0xFF);
  io_wait();
  outb(PIT_CH0_PORT, (divisor >> 8) & 0xFF);

  restore_irq_flags(flags);
}
EXPORT_SYMBOL(pit_set_frequency);

static void pit_wait_internal(uint32_t ms) {
  irq_flags_t flags = save_irq_flags();

  while (ms > 0) {
    uint32_t chunk_ms = (ms > 50) ? 50 : ms;
    ms -= chunk_ms;

    uint16_t count = (uint16_t)((PIT_FREQUENCY_BASE * chunk_ms) / 1000);

    outb(PIT_CMD_PORT, 0x30);
    io_wait();
    outb(PIT_CH0_PORT, count & 0xFF);
    io_wait();
    outb(PIT_CH0_PORT, (count >> 8) & 0xFF);

    while (1) {
      outb(PIT_CMD_PORT, 0x00);
      io_wait();
      uint8_t lo = inb(PIT_CH0_PORT);
      io_wait();
      uint8_t hi = inb(PIT_CH0_PORT);
      uint16_t current_val = ((uint16_t)hi << 8) | lo;

      if (current_val == 0 || current_val > count) {
        break;
      }
      cpu_relax();
    }
  }

  pit_set_frequency(global_pit_frequency);
  restore_irq_flags(flags);
}

static int pit_source_init(void) {
  pit_set_frequency(IC_DEFAULT_TICK);
  return 0;
}

static uint64_t pit_source_get_frequency(void) { return PIT_FREQUENCY_BASE; }

static uint64_t pit_source_read_counter(void) {
  irq_flags_t flags = save_irq_flags();
  outb(PIT_CMD_PORT, 0x00);
  uint8_t lo = inb(PIT_CH0_PORT);
  uint8_t hi = inb(PIT_CH0_PORT);
  restore_irq_flags(flags);

  uint16_t count = ((uint16_t)hi << 8) | lo;
  return (uint64_t)(pit_reload_value - count);
}

static int pit_source_calibrate_tsc(void) {
  uint64_t start = rdtsc();
  pit_wait_internal(50);
  uint64_t end = rdtsc();

  uint64_t freq = (end - start) * 20;
  tsc_recalibrate_with_freq(freq);
  return 0;
}

static time_source_t pit_time_source = {
    .name = "PIT",
    .priority = 100,
    .type = TIME_SOURCE_PIT,
    .init = pit_source_init,
    .get_frequency = pit_source_get_frequency,
    .read_counter = pit_source_read_counter,
    .calibrate_tsc = pit_source_calibrate_tsc,
};

void pit_calibrate_tsc(void) { pit_source_calibrate_tsc(); }

const time_source_t *pit_get_time_source(void) { return &pit_time_source; }

void pit_wait(uint32_t ms) {
  pit_wait_internal(ms);
}

EXPORT_SYMBOL(pit_wait);
EXPORT_SYMBOL(pit_get_time_source);