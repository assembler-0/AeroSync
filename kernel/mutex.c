/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/mutex.c
 * @brief Mutex (Mutual Exclusion) implementation
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

#include <kernel/mutex.h>
#include <kernel/sched/sched.h>
#include <kernel/wait.h>
#include <lib/printk.h>

void mutex_init(mutex_t *m) {
  spinlock_init(&m->lock);
  m->count = 1; /* Unlocked */
  m->owner = NULL;
  init_waitqueue_head(&m->wait_q);
}

void mutex_lock(mutex_t *m) {
  wait_queue_t wait;
  init_wait(&wait);

  /*
   * Optimistic fast path: try to acquire the lock using atomic dec.
   * We use the spinlock to protect the count and owner fields.
   */
  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);

  while (m->count == 0) {
    /* Slow path: block and wait */
    prepare_to_wait(&m->wait_q, &wait, TASK_UNINTERRUPTIBLE);

    /* Check again after preparing to wait, but before scheduling */
    if (m->count != 0) {
      break;
    }

    /* We must release the spinlock before scheduling */
    spinlock_unlock_irqrestore(&m->lock, flags);

    schedule();

    /* After waking up, we re-acquire the spinlock to check condition */
    flags = spinlock_lock_irqsave(&m->lock);
  }

  /* We now have the lock or we broke out because m->count was != 0 */
  m->count = 0;
  m->owner = current;

  /* Clean up wait queue entry */
  finish_wait(&m->wait_q, &wait);

  spinlock_unlock_irqrestore(&m->lock, flags);
}

void mutex_unlock(mutex_t *m) {
  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);

  if (m->owner != current) {
    // Warning: unlocking a mutex we don't own?
    // This is usually a bug, but some kernels allow it for specific cases.
    // For now, let's just warn.
    // printk(KERN_WARNING "mutex_unlock: task %d unlocking mutex owned by
    // %d\n",
    //        current->pid, m->owner ? m->owner->pid : -1);
  }

  m->count = 1;
  m->owner = NULL;

  /* Wake up one waiter (following mutex semantics) */
  wake_up_nr(&m->wait_q, 1);

  spinlock_unlock_irqrestore(&m->lock, flags);
}

int mutex_trylock(mutex_t *m) {
  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);

  if (m->count == 1) {
    m->count = 0;
    m->owner = current;
    spinlock_unlock_irqrestore(&m->lock, flags);
    return 1;
  }

  spinlock_unlock_irqrestore(&m->lock, flags);
  return 0;
}
