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
#include <arch/x64/smp.h>
#include <arch/x64/tsc.h> // Added for get_time_ns
#include <kernel/sched/process.h> 
#include <kernel/sched/sched.h>
#include <drivers/apic/apic.h> // Added for IPI functions
#include <drivers/apic/ic.h>
#include <lib/printk.h>
#include <mm/slab.h>
#include <kernel/classes.h>
#include <linux/container_of.h>

/*
 * Scheduler Core Implementation
 */

// Simple fixed-size runqueue array for now (per CPU)
static struct rq per_cpu_runqueues[MAX_CPUS];

// Current task per CPU (lookup table)
static struct task_struct *current_tasks[MAX_CPUS];

// Preemption flag per CPU
static volatile int need_resched[MAX_CPUS];

// Global scheduler lock for operations that span multiple runqueues (e.g., migration)
spinlock_t __rq_lock = 0; // Initialize unlocked

extern void deactivate_task(struct rq *rq, struct task_struct *p);
extern void activate_task(struct rq *rq, struct task_struct *p);

/*
 * Basic Helpers
 */

int cpu_id(void) {
  // TODO: Map LAPIC ID to logical ID if sparse
  return (int)smp_get_id();
}

struct rq *this_rq(void) { return &per_cpu_runqueues[cpu_id()]; }

struct task_struct *get_current(void) { return current_tasks[cpu_id()]; }

void set_current(struct task_struct *t) { current_tasks[cpu_id()] = t; }

void set_task_cpu(struct task_struct *task, int cpu) {
  task->cpu = cpu;
}


// Internal migration helper - caller must hold __rq_lock
static void __move_task_to_rq_locked(struct task_struct *task, int dest_cpu) {
  struct rq *src_rq = &per_cpu_runqueues[task->cpu];
  struct rq *dest_rq = &per_cpu_runqueues[dest_cpu];

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

  irq_flags_t flags = spinlock_lock_irqsave(&__rq_lock);

  __move_task_to_rq_locked(task, dest_cpu);

  spinlock_unlock_irqrestore(&__rq_lock, flags);
}

/*
 * Scheduler Initialization
 */
void sched_init(void) {
  int i;
  for (i = 0; i < MAX_CPUS; i++) {
    struct rq *rq = &per_cpu_runqueues[i];
    rq->lock = 0; // Init spinlock
    rq->nr_running = 0;
    rq->tasks_timeline = RB_ROOT;
    rq->rb_leftmost = NULL; // Initialize new field
    rq->clock = 0;
    rq->min_vruntime = 0;
  }

  printk(SCHED_CLASS "Scheduler initialized for %d logical CPUs slots.\n",
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
 * Cleanup for the previous task after a context switch.
 * This is called by the new task to clean up the task that just exited.
 */
static void finish_task_switch(struct task_struct *prev) {
  if (prev && prev->state == TASK_ZOMBIE) {
    // We are now running on a different stack, so it's safe to free prev's stack.
    free_task(prev); // This frees both task_struct and its stack
  }
}

/*
 * The main schedule function
 */
void schedule(void) {
  struct task_struct *prev_task, *next_task; // Renamed to avoid confusion
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);
  prev_task = rq->curr;

  // Pick next task
  next_task = pick_next_task_fair(rq);

  if (!next_task) {
    next_task = rq->idle;
  }

  if (prev_task != next_task) {
    rq->curr = next_task;
    set_current(next_task);
    next_task->se.exec_start_ns = get_time_ns(); // Update exec_start_ns for the new task
    spinlock_unlock_irqrestore(&rq->lock, flags); // Release the runqueue lock before switching
    
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
  ic_send_ipi(per_cpu_apic_id[cpu], IRQ_SCHED_IPI_VECTOR, APIC_DELIVERY_MODE_FIXED);
}

// Handler for the scheduler IPI
void irq_sched_ipi_handler(void) {
  need_resched[cpu_id()] = 1; // Signal that a reschedule is needed
  apic_send_eoi(0);           // Acknowledge the interrupt
}

// Simple load balancing function (to be called periodically)
static void load_balance(void) {
  unsigned long total_cpus = smp_get_cpu_count();
  int overloaded_cpu = -1;
  int underloaded_cpu = -1;
  unsigned int max_running = 0;
  unsigned int min_running = UINT32_MAX; // Max value

  if (total_cpus <= 1) {
    return; // No need to load balance on a single CPU
  }

  // Find overloaded and underloaded CPUs
  for (unsigned int i = 0; i < total_cpus; i++) {
    struct rq *rq = &per_cpu_runqueues[i];
    if (rq->nr_running > max_running) {
      max_running = rq->nr_running;
      overloaded_cpu = i;
    }
    if (rq->nr_running < min_running) {
      min_running = rq->nr_running;
      underloaded_cpu = i;
    }
  }

  // Check for significant imbalance (e.g., difference > 1 task)
  if (overloaded_cpu != -1 && underloaded_cpu != -1 &&
      (max_running - min_running > 1)) {
    // Attempt to migrate one task from overloaded to underloaded
    struct rq *src_rq = &per_cpu_runqueues[overloaded_cpu];

    // Acquire the global lock before manipulating runqueues for migration
    irq_flags_t flags = spinlock_lock_irqsave(&__rq_lock);;

    struct task_struct *task_to_migrate = NULL;
    // For simplicity, take the least recently run task (leftmost in rbtree)
    struct rb_node *leftmost = src_rq->rb_leftmost;
    if (leftmost) {
      struct sched_entity *se = rb_entry(leftmost, struct sched_entity, run_node);
      task_to_migrate = container_of(se, struct task_struct, se);
    }

    if (task_to_migrate &&
        task_to_migrate != src_rq->idle &&
        task_to_migrate != src_rq->curr) { // Don't migrate idle or currently running task
      // Perform migration
      __move_task_to_rq_locked(task_to_migrate, underloaded_cpu);
      printk(SCHED_CLASS "Migrated task %p (PID: %d) from CPU %d to %d\n",
             task_to_migrate, task_to_migrate->pid, overloaded_cpu, underloaded_cpu);

      // Signal the destination CPU to reschedule immediately
      reschedule_cpu(underloaded_cpu);
    }
    spinlock_unlock_irqrestore(&__rq_lock, flags);
  }
}

#define LOAD_BALANCE_INTERVAL_TICKS 100

/*
 * Called from timer interrupt
 */
void scheduler_tick(void) {
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

  // Set preemption flag
  need_resched[cpu_id()] = 1;

  spinlock_unlock((volatile int *)&rq->lock);
}

/*
 * Check if preemption is needed and schedule if safe
 */
void check_preempt(void) {
  if (need_resched[cpu_id()]) {
    need_resched[cpu_id()] = 0;
    schedule();
  }
}


/*
 * Initialize the first task (idle/init) for BSP
 */
void sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  
  // Initialize the task's scheduler entity
  initial_task->se.vruntime = 0;
  initial_task->se.on_rq = 0;
  initial_task->state = TASK_RUNNING;
  initial_task->se.exec_start_ns = get_time_ns(); // Initialize exec_start_ns
  initial_task->nice = 0; // Default nice value for initial task
  initial_task->se.load.weight = prio_to_weight[initial_task->nice + NICE_TO_PRIO_OFFSET];
  
  rq->curr = initial_task;
  rq->idle = initial_task;
  set_current(initial_task);
}

/*
 * Scheduler memory statistics
 */
void sched_dump_memory_stats(void) {
  slab_dump_stats();
}
