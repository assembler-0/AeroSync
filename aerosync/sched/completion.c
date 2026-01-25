/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/completion.c
 * @brief Completion synchronization primitive implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <aerosync/completion.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <aerosync/fkx/fkx.h>

void wait_for_completion(struct completion *x) {
  /* Use DEFINE_WAIT from wait.h which sets up wait_queue_t correctly for
   * current task */
  DEFINE_WAIT(wait);

  /* Add to wait queue - generic function handles locking */
  add_wait_queue(&x->wait, &wait);

  for (;;) {
    get_current()->state = TASK_UNINTERRUPTIBLE;

    /* Check condition */
    irq_flags_t flags = spinlock_lock_irqsave(&x->wait.lock);
    if (x->done) {
      x->done--;
      spinlock_unlock_irqrestore(&x->wait.lock, flags);
      break;
    }
    spinlock_unlock_irqrestore(&x->wait.lock, flags);

    schedule();
  }

  get_current()->state = TASK_RUNNING;
  remove_wait_queue(&x->wait, &wait);
}
EXPORT_SYMBOL(wait_for_completion);

unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout) {
  /* Timeout not implemented yet, fall back to infinite wait */
  wait_for_completion(x);
  return 1;
}
EXPORT_SYMBOL(wait_for_completion_timeout);

void complete(struct completion *x) {
  irq_flags_t flags = spinlock_lock_irqsave(&x->wait.lock);
  x->done++;
  wake_up(&x->wait); /* Wakes one */
  spinlock_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete);

void complete_all(struct completion *x) {
  irq_flags_t flags = spinlock_lock_irqsave(&x->wait.lock);
  x->done += 0x7FFFFFFF; /* Large number */
  wake_up_all(&x->wait); /* Wakes all */
  spinlock_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete_all);