#pragma once

#include <arch/x64/cpu.h>

/* Preemption Control */
static inline void preempt_disable(void) {
  struct cpu_data *cpu = get_cpu_data();
  if (cpu) {
    cpu->preempt_count++;
  }
  __asm__ volatile("" ::: "memory");
}

/* Forward declaration to avoid include cycle if possible, 
   but check_preempt is needed by preempt_enable */
void check_preempt(void);

static inline void preempt_enable(void) {
  struct cpu_data *cpu = get_cpu_data();
  __asm__ volatile("" ::: "memory");
  if (cpu) {
    cpu->preempt_count--;
    if (cpu->preempt_count == 0) {
      check_preempt();
    }
  }
}

static inline void preempt_enable_no_resched(void) {
  struct cpu_data *cpu = get_cpu_data();
  __asm__ volatile("" ::: "memory");
  if (cpu) {
    cpu->preempt_count--;
  }
}

static inline int preemptible(void) {
  struct cpu_data *cpu = get_cpu_data();
  return cpu ? (cpu->preempt_count == 0) : 1;
}
