#include <arch/x64/tsc.h>
#include <drivers/apic/ic.h>
#include <drivers/timer/pit.h>
#include <kernel/classes.h>
#include <lib/printk.h>

static uint64_t tsc_freq = 0;
static uint64_t tsc_boot_offset = 0;

uint64_t tsc_freq_get() {
  // Use PIT specific calibration
  // We don't rely on timer_hz_before from argument fr APIC anymore,
  // we specificially use the PIT for calibration block.

  if (tsc_freq == 0) {
    uint64_t start = rdtsc();
    pit_wait(50); // Wait 50ms using PIT busy-wait
    uint64_t end = rdtsc();

    // delta is 50ms worth of ticks
    // freq = delta * (1000 / 50) = delta * 20
    tsc_freq = (end - start) * 20;
    tsc_boot_offset = end; // Use 'end' as the zero point (roughly)
  }

  return tsc_freq;
}

uint64_t get_tsc_freq(void) { return tsc_freq; }

uint64_t calibrate_tsc(void) { return tsc_freq_get(); }

uint64_t get_time_ns() {
  uint64_t now = rdtsc();
  if (now < tsc_boot_offset)
    return 0;
  return ((now - tsc_boot_offset) * 1000000000ULL) / tsc_freq;
}

uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

uint64_t rdtscp(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
  return ((uint64_t)hi << 32) | lo;
}

void tsc_delay(uint64_t ns) {
  uint64_t start = rdtsc();
  uint64_t end;
  uint64_t ticks = (tsc_freq * ns) / 1000000000ULL;
  do {
    end = rdtsc();
  } while ((end - start) < ticks);
}

void tsc_delay_ms(uint64_t ms) { tsc_delay(ms * 1000000ULL); }