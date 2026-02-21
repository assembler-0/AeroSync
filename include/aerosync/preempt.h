#pragma once

#include <aerosync/types.h>
#include <arch/x86_64/percpu.h>
#include <aerosync/compiler.h>

struct task_struct;

extern struct task_struct *get_current(void);

DECLARE_PER_CPU(int, __preempt_count);

/**
 * preempt_count - Get current preemption count
 */
static inline int preempt_count(void) {
  return this_cpu_read(__preempt_count);
}

/**
 * preempt_disable - Disable preemption
 */
static inline void preempt_disable(void) {
  this_cpu_inc(__preempt_count);
  cbarrier();
}

/**
 * preempt_enable_no_resched - Enable preemption without rescheduling
 */
static inline void preempt_enable_no_resched(void) {
  cbarrier();
  this_cpu_dec(__preempt_count);
}

/**
 * in_atomic - Check if we're in atomic context
 */
#define in_atomic() (preempt_count() != 0)

/**
 * preemptible - Check if current context is preemptible
 */
#define preemptible() (preempt_count() == 0)

/* Forward declaration for schedule */
void schedule(void);

DECLARE_PER_CPU(int, need_resched);

/**
 * preempt_enable - Enable preemption and reschedule if needed
 */
static inline void preempt_enable(void) {
  cbarrier();
  int count = this_cpu_read(__preempt_count) - 1;
  this_cpu_write(__preempt_count, count);
  if (count == 0) {
    if (this_cpu_read(need_resched)) {
      schedule();
    }
  }
}
