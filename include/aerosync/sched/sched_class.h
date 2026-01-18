/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sched/sched_class.h
 * @brief Scheduler class abstraction interface
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This implements a Linux-like scheduler class hierarchy where different
 * scheduling policies (CFS, RT, Deadline) can coexist. Classes are ordered
 * by priority: stop > deadline > RT > fair > idle.
 */

#pragma once

#include <aerosync/types.h>

/* Forward declarations */
struct rq;
struct task_struct;
struct cpumask;

/**
 * struct sched_class - Scheduler class operations
 *
 * Each scheduling policy implements this interface. The scheduler iterates
 * through classes in priority order (via @next pointer) when picking the
 * next task to run.
 */
struct sched_class {
  /**
   * @next: Next lower priority scheduler class
   *
   * Forms a linked list: dl_sched_class -> rt_sched_class -> fair_sched_class
   * -> idle_sched_class
   */
  const struct sched_class *next;

  /**
   * @enqueue_task: Add a task to the runqueue
   * @rq: The runqueue
   * @p: Task to enqueue
   * @flags: Enqueue flags (ENQUEUE_WAKEUP, etc.)
   */
  void (*enqueue_task)(struct rq *rq, struct task_struct *p, int flags);

  /**
   * @dequeue_task: Remove a task from the runqueue
   * @rq: The runqueue
   * @p: Task to dequeue
   * @flags: Dequeue flags
   */
  void (*dequeue_task)(struct rq *rq, struct task_struct *p, int flags);

  /**
   * @yield_task: Handle sched_yield() for a task
   * @rq: The runqueue
   */
  void (*yield_task)(struct rq *rq);

  /**
   * @check_preempt_curr: Check if current task should be preempted
   * @rq: The runqueue
   * @p: The waking task
   * @flags: Wake flags
   *
   * Called when a task wakes up to check if it should preempt the current.
   */
  void (*check_preempt_curr)(struct rq *rq, struct task_struct *p, int flags);

  /**
   * @pick_next_task: Select the next task to run
   * @rq: The runqueue
   *
   * Returns the highest priority runnable task for this class, or NULL
   * if no tasks are runnable in this class.
   */
  struct task_struct *(*pick_next_task)(struct rq *rq);

  /**
   * @put_prev_task: Called when a task is about to be switched out
   * @rq: The runqueue
   * @p: Task being switched out
   *
   * Allows the class to update accounting and potentially re-enqueue
   * the task if it's still runnable.
   */
  void (*put_prev_task)(struct rq *rq, struct task_struct *p);

  /**
   * @set_next_task: Called when next task is about to start running
   * @rq: The runqueue
   * @p: Task about to run
   * @first: True if this is the first time picking this task
   */
  void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);

  /**
   * @task_tick: Called on every timer tick for the running task
   * @rq: The runqueue
   * @p: Current task
   * @queued: Whether task is on the runqueue
   */
  void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);

  /**
   * @task_fork: Called when a task forks a new child
   * @p: The new child task
   */
  void (*task_fork)(struct task_struct *p);

  /**
   * @task_dead: Called when a task exits
   * @p: The exiting task
   */
  void (*task_dead)(struct task_struct *p);

  /**
   * @switched_from: Called when task is switching away from this class
   * @rq: The runqueue
   * @p: Task switching classes
   */
  void (*switched_from)(struct rq *rq, struct task_struct *p);

  /**
   * @switched_to: Called when task switches to this class
   * @rq: The runqueue
   * @p: Task that switched
   */
  void (*switched_to)(struct rq *rq, struct task_struct *p);

  /**
   * @prio_changed: Called when task priority changes
   * @rq: The runqueue
   * @p: Task whose priority changed
   * @oldprio: Previous priority
   */
  void (*prio_changed)(struct rq *rq, struct task_struct *p, int oldprio);

  /**
   * @get_rr_interval: Get round-robin time slice
   * @rq: The runqueue
   * @p: Task to query
   *
   * Returns time slice in nanoseconds.
   */
  uint64_t (*get_rr_interval)(struct rq *rq, struct task_struct *p);

  /**
   * @update_curr: Update current task's runtime statistics
   * @rq: The runqueue
   */
  void (*update_curr)(struct rq *rq);

  /**
   * @balance: Perform load balancing for this class
   * @rq: The runqueue
   * @prev: Previous task
   * @rf: Runqueue flags
   *
   * Returns true if load balancing found work.
   */
  int (*balance)(struct rq *rq, struct task_struct *prev, void *rf);

  /**
   * @select_task_rq: Select runqueue for a waking task
   * @p: Task being woken
   * @cpu: Hint CPU
   * @wake_flags: Wake flags
   *
   * Returns the CPU to run the task on.
   */
  int (*select_task_rq)(struct task_struct *p, int cpu, int wake_flags);

  /**
   * @migrate_task_rq: Called when task is migrated to another CPU
   * @p: Task being migrated
   * @new_cpu: Destination CPU
   */
  void (*migrate_task_rq)(struct task_struct *p, int new_cpu);

  /**
   * @task_woken: Called after task has been woken
   * @rq: The runqueue
   * @p: Task that woke up
   */
  void (*task_woken)(struct rq *rq, struct task_struct *p);

  /**
   * @set_cpus_allowed: Update task's CPU affinity
   * @p: Task
   * @newmask: New CPU mask
   */
  void (*set_cpus_allowed)(struct task_struct *p,
                           const struct cpumask *newmask);
};

/* Scheduler class declarations - ordered by priority */
extern const struct sched_class dl_sched_class;   /* Deadline (highest) */
extern const struct sched_class rt_sched_class;   /* Real-Time */
extern const struct sched_class fair_sched_class; /* CFS (normal) */
extern const struct sched_class idle_sched_class; /* Idle (lowest) */

/**
 * sched_class_highest - Get the highest priority scheduler class
 *
 * Returns pointer to the highest priority scheduler class (deadline or RT
 * depending on configuration).
 */
static inline const struct sched_class *sched_class_highest(void) {
  /* Return Deadline class as highest priority */
  return &dl_sched_class;
}

/**
 * for_each_class - Iterate through all scheduler classes by priority
 * @class: Loop variable (const struct sched_class *)
 */
#define for_each_class(class)                                                  \
  for (class = sched_class_highest(); class; class = class->next)

/**
 * rt_prio - Check if priority is in RT range
 * @prio: Priority to check
 *
 * RT priorities are 0-99 (lower number = higher priority).
 */
static inline bool rt_prio(int prio) { return prio < 100; }

/**
 * dl_prio - Check if priority is deadline
 * @prio: Priority to check
 *
 * Deadline tasks have priority -1 (highest possible).
 */
static inline bool dl_prio(int prio) { return prio < 0; }
