/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/wait.c
 * @brief Wait queue implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <arch/x86_64/percpu.h>
#include <arch/x86_64/tsc.h>
#include <aerosync/sched/sched.h>
#include <aerosync/wait.h>
#include <lib/printk.h>
#include <linux/container_of.h>

/*
 * Wait queue implementation for AeroSync kernel
 * Provides Linux-like wait queue functionality integrated with CFS scheduler
 */

// Forward declaration for per_cpu_runqueues
DECLARE_PER_CPU(struct rq, runqueues);

/**
 * default_wake_function - Default wake function for wait queues
 * @wq_head: Wait queue head
 * @wait: Wait queue entry
 * @mode: Sleep mode
 * @key: Wakeup key
 *
 * Returns 1 if the task was woken up, 0 otherwise
 */
int default_wake_function(wait_queue_head_t *wq_head, struct __wait_queue *wait,
                          int mode, unsigned long key) {
  struct task_struct *task = wait->task;
  int ret = 0;

  if (task) {
    // Check if task is still in the appropriate sleep state
    if (task->state == TASK_INTERRUPTIBLE ||
        task->state == TASK_UNINTERRUPTIBLE) {
      // Wake up the task by changing its state to running
      task->state = TASK_RUNNING;

      // Add the task to the runqueue if it's not already there
      struct rq *rq = per_cpu_ptr(runqueues, task->cpu);
      activate_task(rq, task);

      ret = 1;
    }
  }

  return ret;
}

/**
 * add_wait_queue - Add a task to a wait queue
 * @wq_head: Wait queue head
 * @wait: Wait queue entry to add
 */
void add_wait_queue(wait_queue_head_t *wq_head, wait_queue_t *wait) {
  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);
  list_add_tail(&wait->entry, &wq_head->task_list);
  spinlock_unlock_irqrestore(&wq_head->lock, flags);
}

/**
 * remove_wait_queue - Remove a task from a wait queue
 * @wq_head: Wait queue head
 * @wait: Wait queue entry to remove
 */
void remove_wait_queue(wait_queue_head_t *wq_head, wait_queue_t *wait) {
  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);
  list_del(&wait->entry);
  spinlock_unlock_irqrestore(&wq_head->lock, flags);
}

/**
 * prepare_to_wait - Prepare to wait on a wait queue
 * @wq_head: Wait queue head
 * @wait: Wait queue entry
 * @state: State to sleep in (TASK_INTERRUPTIBLE or TASK_UNINTERRUPTIBLE)
 *
 * Returns the time remaining in jiffies or 0 if condition is met
 */
long prepare_to_wait(wait_queue_head_t *wq_head, wait_queue_t *wait,
                     int state) {
  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);

  // Add ourselves to the wait queue if not already there
  // Note: we don't call add_wait_queue here to avoid recursive locking
  if (list_empty(&wait->entry)) {
    list_add_tail(&wait->entry, &wq_head->task_list);
  }

  // Set the task state to sleep
  struct task_struct *curr = get_current();
  if (curr && curr->state == TASK_RUNNING) {
    curr->state = state;
  }

  spinlock_unlock_irqrestore(&wq_head->lock, flags);

  return 0;
}

/**
 * finish_wait - Finish waiting on a wait queue
 * @wq_head: Wait queue head
 * @wait: Wait queue entry
 */
void finish_wait(wait_queue_head_t *wq_head, wait_queue_t *wait) {
  struct task_struct *curr = get_current();
  irq_flags_t flags;

  // Remove ourselves from the wait queue
  flags = spinlock_lock_irqsave(&wq_head->lock);
  list_del(&wait->entry);
  spinlock_unlock_irqrestore(&wq_head->lock, flags);

  // Ensure we're in the running state
  if (curr && curr->state != TASK_RUNNING) {
    curr->state = TASK_RUNNING;
  }
}

/**
 * wake_up - Wake up all tasks waiting on a wait queue
 * @wq_head: Wait queue head
 */
void wake_up(wait_queue_head_t *wq_head) {
  struct __wait_queue *curr, *next;
  struct list_head *pos;
  int nr_woken = 0;

  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);

  struct list_head *n;
  list_for_each_safe(pos, n, &wq_head->task_list) {
    curr = list_entry(pos, struct __wait_queue, entry);

    if (curr->func) {
      int ret = curr->func(wq_head, curr, TASK_NORMAL, 0);
      if (ret)
        nr_woken++;
    }
  }

  spinlock_unlock_irqrestore(&wq_head->lock, flags);
}

/**
 * wake_up_nr - Wake up a specific number of tasks from a wait queue
 * @wq_head: Wait queue head
 * @nr_exclusive: Number of tasks to wake up
 */
void wake_up_nr(wait_queue_head_t *wq_head, int nr_exclusive) {
  struct __wait_queue *curr;
  struct list_head *pos, *tmp;
  int nr_woken = 0;

  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);

  struct list_head *n2;
  list_for_each_safe(pos, n2, &wq_head->task_list) {
    if (nr_woken >= nr_exclusive)
      break;

    curr = list_entry(pos, struct __wait_queue, entry);

    if (curr->func) {
      int ret = curr->func(wq_head, curr, TASK_NORMAL, 0);
      if (ret) {
        nr_woken++;
      }
    }
  }

  spinlock_unlock_irqrestore(&wq_head->lock, flags);
}

/**
 * wake_up_all - Wake up all tasks waiting on a wait queue (alias for wake_up)
 * @wq_head: Wait queue head
 */
void wake_up_all(wait_queue_head_t *wq_head) { wake_up(wq_head); }

/**
 * wake_up_interruptible - Wake up interruptible tasks waiting on a wait queue
 * @wq_head: Wait queue head
 */
void wake_up_interruptible(wait_queue_head_t *wq_head) {
  struct __wait_queue *curr;
  struct list_head *pos;
  int nr_woken = 0;

  irq_flags_t flags = spinlock_lock_irqsave(&wq_head->lock);

  list_for_each(pos, &wq_head->task_list) {
    curr = list_entry(pos, struct __wait_queue, entry);

    // Only wake up tasks that are in interruptible sleep state
    if (curr->task && (curr->task->state == TASK_INTERRUPTIBLE ||
                       curr->task->state == TASK_RUNNING)) {
      if (curr->func) {
        int ret = curr->func(wq_head, curr, TASK_NORMAL, 0);
        if (ret)
          nr_woken++;
      }
    }
  }

  spinlock_unlock_irqrestore(&wq_head->lock, flags);
}

long __wait_event_timeout(wait_queue_head_t *wq, int (*condition)(void *), void *data, long timeout) {
  long start = (long)(get_time_ns() / 1000000ULL);
  long remaining = timeout;
  wait_queue_t wait;
  init_wait(&wait);

  while (1) {
    prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);
    if (condition(data)) {
      break;
    }
    
    long now = (long)(get_time_ns() / 1000000ULL);
    remaining = timeout - (now - start);
    if (remaining <= 0) {
      remaining = 0;
      break;
    }

    schedule();
  }
  
  finish_wait(wq, &wait);
  return remaining;
}

/**
 * sleep_on - Put current task to sleep on a wait queue
 * @wq: Wait queue to sleep on
 */
void sleep_on(wait_queue_head_t *wq) {
  wait_queue_t wait;
  init_wait(&wait);

  prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);
  schedule();
  finish_wait(wq, &wait);
}

/**
 * interruptible_sleep_on - Put current task to interruptible sleep on a wait
 * queue
 * @wq: Wait queue to sleep on
 */
void interruptible_sleep_on(wait_queue_head_t *wq) {
  wait_queue_t wait;
  init_wait(&wait);

  prepare_to_wait(wq, &wait, TASK_INTERRUPTIBLE);
  schedule();
  finish_wait(wq, &wait);
}