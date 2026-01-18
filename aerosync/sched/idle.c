/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/idle.c
 * @brief Idle Task Scheduler Class
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * The idle class is the lowest priority scheduler class. It only runs
 * when no other tasks are runnable. The idle task is per-CPU and
 * never gets enqueued/dequeued in the normal sense.
 */

#include <aerosync/sched/sched.h>
#include <lib/printk.h>

/**
 * enqueue_task_idle - Enqueue idle task (should never be called)
 */
static void enqueue_task_idle(struct rq *rq, struct task_struct *p, int flags) {
  /* Idle task is never enqueued */
}

/**
 * dequeue_task_idle - Dequeue idle task (should never be called)
 */
static void dequeue_task_idle(struct rq *rq, struct task_struct *p, int flags) {
  /* Idle task is never dequeued */
}

/**
 * yield_task_idle - Idle task yield (no-op)
 */
static void yield_task_idle(struct rq *rq) { /* Nothing to do */ }

/**
 * check_preempt_curr_idle - Any task can preempt idle
 */
static void check_preempt_curr_idle(struct rq *rq, struct task_struct *p,
                                    int flags) {
  /* Any normal task should preempt idle */
  set_need_resched();
}

/**
 * pick_next_task_idle - Return the idle task
 *
 * This is only called when no other scheduler class has runnable tasks.
 */
static struct task_struct *pick_next_task_idle(struct rq *rq) {
  /* Always return the idle task */
  return rq->idle;
}

/**
 * put_prev_task_idle - Called when idle task is switched out
 */
static void put_prev_task_idle(struct rq *rq, struct task_struct *p) {
  /* Nothing to do - idle doesn't go on a runqueue */
}

/**
 * set_next_task_idle - Prepare idle task to run
 */
static void set_next_task_idle(struct rq *rq, struct task_struct *p,
                               bool first) {
  /* Nothing special needed */
}

/**
 * task_tick_idle - Timer tick for idle task
 *
 * Checks if there's now work to do and requests reschedule.
 */
static void task_tick_idle(struct rq *rq, struct task_struct *p, int queued) {
  /* If there are runnable tasks, request reschedule */
  if (rq->nr_running > 0) {
    set_need_resched();
  }
}

/**
 * task_fork_idle - Idle task fork (should never happen)
 */
static void task_fork_idle(struct task_struct *p) {
  /* Idle task should never fork */
}

/**
 * task_dead_idle - Idle task exit (should never happen)
 */
static void task_dead_idle(struct task_struct *p) {
  /* Idle task should never exit */
}

/**
 * switched_to_idle - Task switching to idle class (unusual)
 */
static void switched_to_idle(struct rq *rq, struct task_struct *p) {
  /* This shouldn't normally happen */
}

/**
 * prio_changed_idle - Idle task priority change (no-op)
 */
static void prio_changed_idle(struct rq *rq, struct task_struct *p,
                              int oldprio) {
  /* Idle has no priority to change */
}

/**
 * The idle scheduler class definition
 *
 * This is the lowest priority class and has no next pointer.
 */
const struct sched_class idle_sched_class = {
    .next = NULL, /* Lowest priority - no next class */

    .enqueue_task = enqueue_task_idle,
    .dequeue_task = dequeue_task_idle,
    .yield_task = yield_task_idle,
    .check_preempt_curr = check_preempt_curr_idle,

    .pick_next_task = pick_next_task_idle,
    .put_prev_task = put_prev_task_idle,
    .set_next_task = set_next_task_idle,

    .task_tick = task_tick_idle,
    .task_fork = task_fork_idle,
    .task_dead = task_dead_idle,

    .switched_to = switched_to_idle,
    .prio_changed = prio_changed_idle,

    .get_rr_interval = NULL,
    .update_curr = NULL,
};
