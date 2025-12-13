#include <lib/printk.h>
#include <arch/x64/tsc.h>
#include <drivers/apic/ic.h>
#include <kernel/classes.h>

static uint64_t tsc_freq = 0;

uint64_t tsc_freq_get(const uint32_t timer_hz_before) {
  ic_set_timer(100);
  if (tsc_freq == 0) {
    uint64_t start = rdtsc();
    uint64_t end;
    do {
      end = rdtsc();
    } while (start == end);
    tsc_freq = (end - start) * 100;
  }
  ic_set_timer(timer_hz_before);
  printk(TSC_CLASS "TSC frequency calibrated: %llu Hz\n", tsc_freq);
  return tsc_freq;
}

uint64_t get_tsc_freq(void) {
  return tsc_freq;
}

uint64_t calibrate_tsc(void) {
  return tsc_freq_get(ic_get_frequency());
}

uint64_t get_time_ns() {
  return (rdtsc() * 1000000000ULL) / tsc_freq;
}

uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

uint64_t rdtscp(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
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

void tsc_delay_ms(uint64_t ms) {
  tsc_delay(ms * 1000000ULL);
}