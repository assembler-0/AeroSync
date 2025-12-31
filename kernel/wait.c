/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/wait.c
 * @brief Wait queue implementation
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

#include <arch/x64/percpu.h>
#include <arch/x64/tsc.h>
#include <kernel/sched/sched.h>
#include <kernel/wait.h>
#include <lib/printk.h>
#include <linux/container_of.h>

/*
 * Wait queue implementation for VoidFrameX kernel
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
  if (list_empty(&wait->entry)) {
    add_wait_queue(wq_head, wait);
  }

  // Set the task state to sleep
  struct task_struct *curr = get_current();
  if (curr->state == TASK_RUNNING) {
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
 * __wait_event - Wait for a condition to become true
 * @wq: Wait queue to wait on
 * @condition: Condition to wait for
 */
long __wait_event(wait_queue_head_t *wq, int condition) {
  wait_queue_t wait;
  init_wait(&wait);

  while (1) {
    if (condition)
      break;

    prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);

    if (condition)
      break;

    schedule();
  }

  finish_wait(wq, &wait);
  return 0;
}

/**
 * __wait_event_interruptible - Wait for a condition with interrupt support
 * @wq: Wait queue to wait on
 * @condition: Condition to wait for
 */
long __wait_event_interruptible(wait_queue_head_t *wq, int condition) {
  wait_queue_t wait;
  init_wait(&wait);
  int ret = 0;

  while (1) {
    if (condition)
      break;

    prepare_to_wait(wq, &wait, TASK_INTERRUPTIBLE);

    if (condition)
      break;

    schedule();

    // Check if we were interrupted
    if (get_current()->state == TASK_RUNNING) {
      // We were woken up by a signal or interrupt
      if (!condition) {
        ret = -1; // Indicate interruption
        break;
      }
    }
  }

  finish_wait(wq, &wait);
  return ret;
}

/**
 * __wait_event_timeout - Wait for a condition with timeout
 * @wq: Wait queue to wait on
 * @condition: Condition to wait for
 * @timeout: Timeout in jiffies (currently treated as seconds for simplicity)
 */
long __wait_event_timeout(wait_queue_head_t *wq, int condition, long timeout) {
  wait_queue_t wait;
  init_wait(&wait);
  uint64_t start_time = get_time_ns();
  // Convert timeout from jiffies/seconds to nanoseconds (simplified as 1 jiffy
  // = 1 second)
  uint64_t timeout_ns = timeout * 1000000000ULL;
  uint64_t elapsed_ns;
  uint64_t current_time;

  while (timeout > 0) {
    if (condition)
      break;

    prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);

    if (condition)
      break;

    schedule();

    current_time = get_time_ns();
    elapsed_ns = current_time - start_time;

    if (elapsed_ns >= timeout_ns) {
      timeout = 0;
      break;
    } else {
      // Calculate remaining time in jiffies/seconds
      timeout = (timeout_ns - elapsed_ns) / 1000000000ULL;
    }
  }

  finish_wait(wq, &wait);
  return timeout;
}

/**
 * __wait_event_interruptible_timeout - Wait for a condition with timeout and
 * interrupt support
 * @wq: Wait queue to wait on
 * @condition: Condition to wait for
 * @timeout: Timeout in jiffies (currently treated as seconds for simplicity)
 */
long __wait_event_interruptible_timeout(wait_queue_head_t *wq, int condition,
                                        long timeout) {
  wait_queue_t wait;
  init_wait(&wait);
  uint64_t start_time = get_time_ns();
  // Convert timeout from jiffies/seconds to nanoseconds (simplified as 1 jiffy
  // = 1 second)
  uint64_t timeout_ns = timeout * 1000000000ULL;
  uint64_t elapsed_ns;
  uint64_t current_time;
  int ret = 0;

  while (timeout > 0) {
    if (condition) {
      ret = timeout;
      break;
    }

    prepare_to_wait(wq, &wait, TASK_INTERRUPTIBLE);

    if (condition) {
      ret = timeout;
      break;
    }

    schedule();

    // Check if we were interrupted
    if (get_current()->state == TASK_RUNNING) {
      // We were woken up by a signal or interrupt
      if (!condition) {
        ret = -1; // Indicate interruption
        break;
      }
    }

    current_time = get_time_ns();
    elapsed_ns = current_time - start_time;

    if (elapsed_ns >= timeout_ns) {
      ret = 0; // Timeout
      break;
    } else {
      // Calculate remaining time in jiffies/seconds
      timeout = (timeout_ns - elapsed_ns) / 1000000000ULL;
    }

    if (timeout <= 0) {
      ret = 0; // Timeout
      break;
    }
  }

  finish_wait(wq, &wait);
  return ret;
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