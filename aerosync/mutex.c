/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/mutex.c
 * @brief Mutex (Mutual Exclusion) implementation
 * @copyright (C) 2025-2026 assembler-0
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

#include <aerosync/mutex.h>
#include <aerosync/sched/sched.h>
#include <aerosync/wait.h>
#include <aerosync/fkx/fkx.h>
#include <linux/container_of.h>

void mutex_init(mutex_t *m) {
  spinlock_init(&m->lock);
  m->count = 1; /* Unlocked */
  m->owner = nullptr;
  init_waitqueue_head(&m->wait_q);
  INIT_LIST_HEAD(&m->waiters);
  m->pi_enabled = true;
}
EXPORT_SYMBOL(mutex_init);

void mutex_lock(mutex_t *m) {
  struct task_struct *curr = current;
  wait_queue_entry_t wait;

  if (unlikely(!curr)) {
    /* Early boot: no scheduler yet. Spin until lock acquired. */
    irq_flags_t flags = spinlock_lock_irqsave(&m->lock);
    while (m->count == 0) {
      spinlock_unlock_irqrestore(&m->lock, flags);
      cpu_relax();
      flags = spinlock_lock_irqsave(&m->lock);
    }
    m->count = 0;
    m->owner = nullptr;
    spinlock_unlock_irqrestore(&m->lock, flags);
    return;
  }

  init_wait(&wait);

  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);

  while (m->count == 0) {
    struct task_struct *owner = m->owner;

    /* PI Logic: Boost owner's priority */
    if (m->pi_enabled && owner) {
      curr->pi_blocked_on = m;
      pi_boost_prio(owner, curr);
    }

    /* Slow path: block and wait */
    prepare_to_wait(&m->wait_q, &wait, TASK_UNINTERRUPTIBLE);

    if (m->count != 0) {
      break;
    }

    spinlock_unlock_irqrestore(&m->lock, flags);
    schedule();
    flags = spinlock_lock_irqsave(&m->lock);
  }

  /* Cleanup PI state if we were blocked */
  if (curr->pi_blocked_on == m) {
    curr->pi_blocked_on = nullptr;
  }

  m->count = 0;
  m->owner = curr;

  finish_wait(&m->wait_q, &wait);
  spinlock_unlock_irqrestore(&m->lock, flags);
}
EXPORT_SYMBOL(mutex_lock);

void mutex_unlock(mutex_t *m) {
  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);
  struct task_struct *curr = current;

  if (curr && m->owner != curr) {
    /* Warning handled in professional kernels by BUG_ON or similar */
  }

  /* PI Logic: Restore priority if we were boosted */
  if (curr && m->pi_enabled) {
    bool changed = false;
    irq_flags_t pflags = spinlock_lock_irqsave(&curr->pi_lock);
    struct task_struct *waiter, *tmp;
    list_for_each_entry_safe(waiter, tmp, &curr->pi_waiters, pi_list) {
      if (waiter->pi_blocked_on == m) {
        list_del_init(&waiter->pi_list);
        changed = true;
      }
    }
    
    if (changed) {
      __update_task_prio(curr);
    }
    spinlock_unlock_irqrestore(&curr->pi_lock, pflags);
  }

  m->count = 1;
  m->owner = nullptr;

  /* Wake up one waiter */
  wake_up_nr(&m->wait_q, 1);

  spinlock_unlock_irqrestore(&m->lock, flags);
}
EXPORT_SYMBOL(mutex_unlock);

int mutex_trylock(mutex_t *m) {
  irq_flags_t flags = spinlock_lock_irqsave(&m->lock);
  struct task_struct *curr = current;

  if (m->count == 1) {
    m->count = 0;
    m->owner = curr;
    spinlock_unlock_irqrestore(&m->lock, flags);
    return 1;
  }

  spinlock_unlock_irqrestore(&m->lock, flags);
  return 0;
}
EXPORT_SYMBOL(mutex_trylock);
