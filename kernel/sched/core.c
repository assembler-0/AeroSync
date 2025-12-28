/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sched/core.c
 * @brief Core scheduler implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <arch/x64/cpu.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/percpu.h>
#include <arch/x64/smp.h>
#include <arch/x64/tsc.h>      // Added for get_time_ns
#include <drivers/apic/apic.h> // Added for IPI functions
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <kernel/sysintf/ic.h>
#include <kernel/wait.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <vsprintf.h>

/*
 * Scheduler Core Implementation
 */

// Per-CPU runqueue
DEFINE_PER_CPU(struct rq, runqueues);

// Current task per CPU
DEFINE_PER_CPU(struct task_struct *, current_task);

// Idle task per CPU
DEFINE_PER_CPU(struct task_struct, idle_task);

// Preemption flag per CPU
DEFINE_PER_CPU(int, need_resched);

void set_need_resched(void) { this_cpu_write(need_resched, 1); }

void double_rq_lock(struct rq *rq1, struct rq *rq2) {
  if (rq1 == rq2) {
    spinlock_lock(&rq1->lock);
  } else if (rq1 < rq2) {
    spinlock_lock(&rq1->lock);
    spinlock_lock(&rq2->lock);
  } else {
    spinlock_lock(&rq2->lock);
    spinlock_lock(&rq1->lock);
  }
}

void double_rq_unlock(struct rq *rq1, struct rq *rq2) {
  spinlock_unlock(&rq1->lock);
  if (rq1 != rq2) {
    spinlock_unlock(&rq2->lock);
  }
}

extern void deactivate_task(struct rq *rq, struct task_struct *p);
extern void activate_task(struct rq *rq, struct task_struct *p);

/*
 * Basic Helpers
 */

int cpu_id(void) {
  // TODO: Map LAPIC ID to logical ID if sparse
  return (int)smp_get_id();
}

struct rq *this_rq(void) { return this_cpu_ptr(runqueues); }

struct task_struct *get_current(void) { return this_cpu_read(current_task); }

void set_current(struct task_struct *t) { this_cpu_write(current_task, t); }

void set_task_cpu(struct task_struct *task, int cpu) { task->cpu = cpu; }

// Internal migration helper - caller must hold __rq_lock
static void __move_task_to_rq_locked(struct task_struct *task, int dest_cpu) {
  struct rq *src_rq = per_cpu_ptr(runqueues, task->cpu);
  struct rq *dest_rq = per_cpu_ptr(runqueues, dest_cpu);

  deactivate_task(src_rq, task);
  set_task_cpu(task, dest_cpu);
  activate_task(dest_rq, task);
}

/*
 * Moves a task from its current runqueue to a destination CPU's runqueue.
 * This function handles locking for both runqueues involved.
 */
void move_task_to_rq(struct task_struct *task, int dest_cpu) {
  if (dest_cpu < 0 || dest_cpu >= MAX_CPUS) {
    printk(SCHED_CLASS "Invalid dest_cpu %d in move_task_to_rq\n", dest_cpu);
    return;
  }

  struct rq *src_rq = per_cpu_ptr(runqueues, task->cpu);
  struct rq *dest_rq = per_cpu_ptr(runqueues, dest_cpu);

  irq_flags_t flags = save_irq_flags();
  cpu_cli();

  double_rq_lock(src_rq, dest_rq);

  __move_task_to_rq_locked(task, dest_cpu);

  double_rq_unlock(src_rq, dest_rq);
  restore_irq_flags(flags);
}

static void switch_mm(struct mm_struct *prev, struct mm_struct *next,
                      struct task_struct *tsk) {
  if (prev == next)
    return;

  if (next && next->pml4) {
    vmm_switch_pml4((uint64_t)next->pml4);
  } else {
    vmm_switch_pml4(g_kernel_pml4);
  }
}

/*
 * Scheduler Initialization
 */
void sched_init(void) {
  pid_allocator_init();

  // Per-CPU variables are zero-initialized by setup_per_cpu_areas

  for (int i = 0; i < MAX_CPUS; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    rq->tasks_timeline = RB_ROOT;
  }

  printk(SCHED_CLASS "CFS scheduler initialized for %d logical CPUs slots.\n",
         MAX_CPUS);
}

/*
 * External function to pick the next task (CFS)
 * Implemented in fair.c
 */
extern struct task_struct *pick_next_task_fair(struct rq *rq);
extern void task_tick_fair(struct rq *rq, struct task_struct *curr);

/*
 * Context Switch
 */
extern struct task_struct *switch_to(struct task_struct *prev,
                                     struct task_struct *next);

/*
 * Task state management functions
 */

/**
 * task_sleep - Put current task to sleep
 */
void task_sleep(void) {
  struct task_struct *curr = get_current();
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  // Update runtime before sleeping
  if (rq->curr == curr) {
    update_curr(rq);
  }

  // Deactivate the task from the runqueue
  deactivate_task(rq, curr);

  // Task is now sleeping, so it's not on the runqueue
  spinlock_unlock_irqrestore(&rq->lock, flags);

  // Schedule another task
  schedule();
}

/**
 * task_wake_up - Wake up a specific task
 * @task: Task to wake up
 */
void task_wake_up(struct task_struct *task) {
  struct rq *rq = per_cpu_ptr(runqueues, task->cpu);

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  // If task was sleeping, wake it up by changing its state
  if (task->state == TASK_INTERRUPTIBLE ||
      task->state == TASK_UNINTERRUPTIBLE) {
    task->state = TASK_RUNNING;
    activate_task(rq, task);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

/**
 * task_wake_up_all - Wake up all tasks (for system events)
 */
void task_wake_up_all(void) {
  // This function would iterate through all CPUs and wake up tasks
  // For now, we'll implement a simple version
  int cpu;
  for (cpu = 0; cpu < smp_get_cpu_count(); cpu++) {
    struct rq *rq = per_cpu_ptr(runqueues, cpu);
    // Wake up any sleeping tasks on this CPU
    // Implementation would depend on how tasks are tracked when sleeping
  }
}

/*
 * Cleanup for the previous task after a context switch.
 * This is called by the new task to clean up the task that just exited.
 */
static void finish_task_switch(struct task_struct *prev) {
  if (prev && prev->state == TASK_ZOMBIE) {
    // We are now running on a different stack, so it's safe to free prev's
    // stack.
    free_task(prev); // This frees both task_struct and its stack
  }
}

void set_task_nice(struct task_struct *p, int nice) {
  if (nice < MIN_NICE)
    nice = MIN_NICE;
  if (nice > MAX_NICE)
    nice = MAX_NICE;

  if (p->nice == nice)
    return;

  struct rq *rq = per_cpu_ptr(runqueues, p->cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  // Update runtime before changing weight to be fair
  if (rq->curr == p) {
    update_curr(rq);
  }

  int queued = p->se.on_rq;
  if (queued) {
    deactivate_task(rq, p);
  }

  p->nice = nice;
  p->se.load.weight = prio_to_weight[nice + NICE_TO_PRIO_OFFSET];

  if (queued) {
    activate_task(rq, p);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

/*
 * The main schedule function
 */
void schedule(void) {
  struct task_struct *prev_task, *next_task; // Renamed to avoid confusion
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);
  prev_task = rq->curr;

  if (prev_task) {
    update_curr(rq);
    put_prev_task_fair(rq, prev_task);
  }

  // Pick next task
  next_task = pick_next_task_fair(rq);

  if (!next_task) {
    next_task = rq->idle;
    // Idle task is never in the tree, so no need to dequeue it in
    // pick_next_task_fair and no need to enqueue it in put_prev_task_fair
    // (handled by on_rq or special cases)
  }

  if (!next_task) {
    panic("schedule(): next_task is NULL (rq->idle)");
  }

  next_task->se.exec_start_ns = get_time_ns();
  next_task->se.prev_sum_exec_runtime = next_task->se.sum_exec_runtime;

  if (prev_task != next_task) {
    rq->curr = next_task;
    set_current(next_task);

    // Handle address space switching
    if (next_task->mm) {
      switch_mm(prev_task->active_mm, next_task->mm, next_task);
      next_task->active_mm = next_task->mm;
    } else {
      // Kernel thread inherits the active_mm of the previous task
      next_task->active_mm = prev_task->active_mm;
      // Optimization: if it's the same active_mm, switch_mm will skip
      switch_mm(prev_task->active_mm, next_task->active_mm, next_task);
    }

    spinlock_unlock_irqrestore(
        &rq->lock, flags); // Release the runqueue lock before switching

    // switch_to returns the task that was just switched *from*
    prev_task = switch_to(prev_task, next_task);

    // Now we are running in the context of 'next_task'
    finish_task_switch(prev_task); // Clean up the truly previous task
    return;
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

// Function to signal a remote CPU to reschedule
void reschedule_cpu(int cpu) {
  ic_send_ipi(*per_cpu_ptr(cpu_apic_id, cpu), IRQ_SCHED_IPI_VECTOR,
              APIC_DELIVERY_MODE_FIXED);
}

// Handler for the scheduler IPI
void irq_sched_ipi_handler(void) {
  this_cpu_write(need_resched, 1); // Signal that a reschedule is needed
                                   // No EOI here, irq_common_stub handles it
}

static void load_balance(void) {
  unsigned long total_cpus = smp_get_cpu_count();
  int overloaded_cpu = -1;
  int underloaded_cpu = -1;
  unsigned long max_load = 0;
  unsigned long min_load = ~0UL;

  if (total_cpus <= 1) {
    return; // No need to load balance on a single CPU
  }

  /* Find overloaded and underloaded CPUs based on average load */
  for (unsigned int i = 0; i < total_cpus; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    unsigned long load = rq->avg_load;
    if (load > max_load) {
      max_load = load;
      overloaded_cpu = (signed)i;
    }
    if (load < min_load) {
      min_load = load;
      underloaded_cpu = (signed)i;
    }
  }

  /* Only act on a significant imbalance */
  if (overloaded_cpu == -1 || underloaded_cpu == -1 ||
      (max_load - min_load <= NICE_0_LOAD)) {
    return;
  }

  struct rq *src_rq = per_cpu_ptr(runqueues, overloaded_cpu);
  struct rq *dst_rq = per_cpu_ptr(runqueues, underloaded_cpu);

  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  double_rq_lock(src_rq, dst_rq);

  /* Re-evaluate imbalance under locks */
  if (src_rq->avg_load - dst_rq->avg_load <= NICE_0_LOAD) {
    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);
    return;
  }

  /* Find a candidate task on the source runqueue */
  struct rb_node *n = src_rq->rb_leftmost ? src_rq->rb_leftmost
                                          : rb_first(&src_rq->tasks_timeline);
  struct task_struct *candidate = NULL;

  for (; n; n = rb_next(n)) {
    struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
    struct task_struct *t = container_of(se, struct task_struct, se);

    if (t == src_rq->idle || t == src_rq->curr)
      continue;
    if (!se->on_rq)
      continue;

    candidate = t;
    break;
  }

  if (candidate) {
    __move_task_to_rq_locked(candidate, underloaded_cpu);
    printk(KERN_DEBUG SCHED_CLASS
           "Migrated task %p (PID: %d) from CPU %d to %d (load: %lu -> %lu)\n",
           candidate, candidate->pid, overloaded_cpu, underloaded_cpu, max_load,
           min_load);

    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);

    /* Signal destination CPU to reschedule */
    reschedule_cpu(underloaded_cpu);
  } else {
    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);
  }
}

#define LOAD_BALANCE_INTERVAL_TICKS 100

/*
 * Called from timer interrupt
 */
void __hot scheduler_tick(void) {
  struct rq *rq = this_rq();
  struct task_struct *curr = rq->curr;

  spinlock_lock((volatile int *)&rq->lock);

  rq->clock++;

  if (curr) {
    task_tick_fair(rq, curr);
  }

  // Perform load balancing periodically
  if (rq->clock % LOAD_BALANCE_INTERVAL_TICKS == 0) {
    load_balance();
  }

  spinlock_unlock((volatile int *)&rq->lock);
}

/*
 * Check if preemption is needed and schedule if safe
 */
void check_preempt(void) {
  if (this_cpu_read(need_resched)) {
    this_cpu_write(need_resched, 0);
    schedule();
  }
}

/*
 * Initialize the first task (idle/init) for a single CPU
 */
void sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  initial_task->mm = &init_mm;
  initial_task->active_mm = &init_mm;

  // Initialize the task's scheduler entity
  initial_task->se.vruntime = 0;
  initial_task->se.on_rq = 0;
  initial_task->state = TASK_RUNNING;
  initial_task->se.exec_start_ns = get_time_ns(); // Initialize exec_start_ns
  initial_task->nice = MAX_NICE; // Default nice value for idle task
  initial_task->se.load.weight =
      prio_to_weight[initial_task->nice + NICE_TO_PRIO_OFFSET];

  rq->curr = initial_task;
  rq->idle = initial_task;
  set_current(initial_task);
}

void sched_init_ap(void) {
  int cpu = cpu_id();
  struct task_struct *idle = per_cpu_ptr(idle_task, cpu);

  memset(idle, 0, sizeof(*idle));
  snprintf(idle->comm, sizeof(idle->comm), "idle/%d", cpu);
  idle->cpu = cpu;
  idle->flags = PF_KTHREAD;

  sched_init_task(idle);
}