/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/softirq.c
 * @brief SoftIRQ (Bottom Half) processing system
 */

#include <aerosync/softirq.h>
#include <aerosync/sched/sched.h>
#include <aerosync/panic.h>
#include <arch/x86_64/cpu.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/sched/process.h>

#define MAX_SOFTIRQ_RESTART 10

DEFINE_PER_CPU(uint32_t, softirq_pending);
DEFINE_PER_CPU(int, softirq_nesting);
DEFINE_PER_CPU(int, hardirq_nesting);
static DEFINE_PER_CPU(struct task_struct *, ksoftirqd_task);

static struct softirq_action softirq_vec[NR_SOFTIRQS];

/* Foward declaration */
static void __do_softirq(void);

static int ksoftirqd_should_run(unsigned int cpu) {
  return *per_cpu_ptr(softirq_pending, cpu) != 0;
}

static int ksoftirqd_thread(void *data) {
  (void) data;
  while (1) {
    if (!ksoftirqd_should_run(smp_get_id())) {
      schedule();
      continue;
    }

    while (this_cpu_read(softirq_pending)) {
      __do_softirq();
      cpu_relax();
    }
  }
  return 0;
}

void open_softirq(int nr, void (*action)(struct softirq_action *)) {
  if (nr < NR_SOFTIRQS) {
    softirq_vec[nr].action = action;
  }
}

static void wakeup_softirqd(void) {
  struct task_struct *tsk = this_cpu_read(ksoftirqd_task);
  if (tsk && tsk->state != TASK_RUNNING) {
    task_wake_up(tsk);
  }
}

void raise_softirq(unsigned int nr) {
  if (nr < NR_SOFTIRQS) {
    this_cpu_write(softirq_pending, this_cpu_read(softirq_pending) | (1 << nr));
    if (!in_interrupt()) {
      wakeup_softirqd();
    }
  }
}

/**
 * __do_softirq - Internal softirq processing loop
 */
static void __do_softirq(void) {
  uint32_t pending;
  int max_restart = MAX_SOFTIRQ_RESTART;
  struct softirq_action *h;

  pending = this_cpu_read(softirq_pending);
  this_cpu_write(softirq_pending, 0);

  this_cpu_write(softirq_nesting, this_cpu_read(softirq_nesting) + 1);

restart:
  h = softirq_vec;
  unsigned int nr = 0;

  while (pending) {
    if (pending & 1) {
      if (h->action) {
        h->action(h);
      }
    }
    h++;
    pending >>= 1;
    nr++;
  }

  pending = this_cpu_read(softirq_pending);
  if (pending && --max_restart) {
    this_cpu_write(softirq_pending, 0);
    goto restart;
  }

  if (pending) {
    wakeup_softirqd();
  }

  this_cpu_write(softirq_nesting, this_cpu_read(softirq_nesting) - 1);
}

void invoke_softirq(void) {
  if (!in_interrupt() && this_cpu_read(softirq_pending)) {
    __do_softirq();
  }
}

void irq_enter(void) {
  this_cpu_write(hardirq_nesting, this_cpu_read(hardirq_nesting) + 1);
}

void irq_exit(void) {
  this_cpu_write(hardirq_nesting, this_cpu_read(hardirq_nesting) - 1);

  if (!in_interrupt() && this_cpu_read(softirq_pending)) {
    __do_softirq();
  }

  /* Preemption check on IRQ exit */
  if (!in_interrupt() && this_cpu_read(need_resched)) {
    schedule();
  }
}

bool in_interrupt(void) {
  return this_cpu_read(hardirq_nesting) > 0 || this_cpu_read(softirq_nesting) > 0;
}

bool in_softirq(void) {
  return this_cpu_read(softirq_nesting) > 0;
}

void softirq_init_ap(void) {
  struct task_struct *tsk = kthread_create(ksoftirqd_thread, nullptr, "ksoftirqd/%d", smp_get_id());
  if (tsk) {
    this_cpu_write(ksoftirqd_task, tsk);
    /* Set affinity to the current CPU */
    cpumask_clear(&tsk->cpus_allowed);
    cpumask_set_cpu(smp_get_id(), &tsk->cpus_allowed);
    kthread_run(tsk);
  }
}

void softirq_init(void) {
  for (int i = 0; i < MAX_CPUS; i++) {
    *per_cpu_ptr(softirq_pending, i) = 0;
    *per_cpu_ptr(softirq_nesting, i) = 0;
    *per_cpu_ptr(hardirq_nesting, i) = 0;
  }

  // Spawn ksoftirqd for BSP
  struct task_struct *tsk = kthread_create(ksoftirqd_thread, nullptr, "ksoftirqd/%d", smp_get_id());
  if (tsk) {
    this_cpu_write(ksoftirqd_task, tsk);
    kthread_run(tsk);
  }

  printk(KERN_INFO IRQ_CLASS "softirq initialized.\n");
}
