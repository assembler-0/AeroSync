///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/rcu.c
 * @brief Read-Copy-Update (RCU) implementation
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

#include <linux/rcupdate.h>
#include <arch/x86_64/percpu.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sched/process.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/classes.h>

/**
 * struct rcu_ctrl - Global RCU state
 */
struct rcu_ctrl {
  spinlock_t lock;
  uint64_t cur_gp; /* Current grace period number */
  uint64_t completed_gp; /* Last completed grace period number */
  cpumask_t cpumask; /* CPUs that must pass through quiescent state */

  wait_queue_head_t gp_wait;
};

static struct rcu_ctrl rcu_state;

/**
 * struct rcu_data - Per-CPU RCU state
 */
struct rcu_data {
  uint64_t gp_num; /* GP number this CPU is waiting for */
  bool qs_pending; /* True if this CPU needs a quiescent state */

  struct rcu_head *callbacks;
  struct rcu_head **callbacks_tail;

  struct rcu_head *wait_callbacks;
  struct rcu_head **wait_tail;
};

static DEFINE_PER_CPU(struct rcu_data, rcu_percpu_data);

void rcu_init(void) {
  memset(&rcu_state, 0, sizeof(rcu_state));
  spinlock_init(&rcu_state.lock);
  init_waitqueue_head(&rcu_state.gp_wait);

  int cpu;
  for_each_possible_cpu(cpu) {
    struct rcu_data *rdp = per_cpu_ptr(rcu_percpu_data, cpu);
    memset(rdp, 0, sizeof(*rdp));
    rdp->callbacks_tail = &rdp->callbacks;
    rdp->wait_tail = &rdp->wait_callbacks;
  }

  printk(KERN_INFO SYNC_CLASS "Classic Multi-CPU RCU Initialized\n");
}

/**
 * rcu_start_gp - Start a new grace period
 *
 * Must be called with rcu_state.lock held.
 */
static void rcu_start_gp(void) {
  if (rcu_state.cur_gp != rcu_state.completed_gp)
    return; /* GP already in progress */

  rcu_state.cur_gp++;
  cpumask_copy(&rcu_state.cpumask, &cpu_online_mask);

  int cpu;
  for_each_online_cpu(cpu) {
    struct rcu_data *rdp = per_cpu_ptr(rcu_percpu_data, cpu);
    rdp->gp_num = rcu_state.cur_gp;
    rdp->qs_pending = true;
  }
}

/**
 * rcu_qs - Report a quiescent state for the current CPU
 */
void rcu_qs(void) {
  struct rcu_data *rdp = this_cpu_ptr(rcu_percpu_data);

  if (!rdp->qs_pending) return;

  irq_flags_t flags = spinlock_lock_irqsave(&rcu_state.lock);

  /* Re-check under lock */
  if (rdp->qs_pending && rdp->gp_num == rcu_state.cur_gp) {
    cpumask_clear_cpu(smp_get_id(), &rcu_state.cpumask);
    rdp->qs_pending = false;

    if (cpumask_empty(&rcu_state.cpumask)) {
      rcu_state.completed_gp = rcu_state.cur_gp;
      wake_up_all(&rcu_state.gp_wait);
    }
  }

  spinlock_unlock_irqrestore(&rcu_state.lock, flags);
}

/**
 * rcu_process_callbacks - Invoke ready callbacks
 */
static void rcu_process_callbacks(void) {
  struct rcu_head *list = NULL;
  irq_flags_t flags = local_irq_save();
  struct rcu_data *rdp = this_cpu_ptr(rcu_percpu_data);

  /* 1. If current wait list finished its GP, move to local list for execution */
  if (rdp->wait_callbacks && rcu_state.completed_gp >= rdp->gp_num) {
    list = rdp->wait_callbacks;
    rdp->wait_callbacks = NULL;
    rdp->wait_tail = &rdp->wait_callbacks;
  }

  /* 2. If no wait list pending, move new callbacks to wait list and start GP */
  if (!rdp->wait_callbacks && rdp->callbacks) {
    spinlock_lock(&rcu_state.lock);

    rdp->wait_callbacks = rdp->callbacks;
    rdp->wait_tail = rdp->callbacks_tail;
    rdp->callbacks = NULL;
    rdp->callbacks_tail = &rdp->callbacks;

    /* Start GP for these callbacks */
    rcu_start_gp();
    rdp->gp_num = rcu_state.cur_gp;
    rdp->qs_pending = true;

    spinlock_unlock(&rcu_state.lock);
  }

  local_irq_restore(flags);

  /* 3. Execute ready callbacks */
  while (list) {
    struct rcu_head *next = list->next;
    list->func(list);
    list = next;
  }
}

/**
 * rcu_check_callbacks - Called from scheduler tick
 */
void rcu_check_callbacks(void) {
  /* If we are preemptible, we are in a quiescent state */
  if (preemptible()) {
    rcu_qs();
  }

  /* Process callbacks asynchronously */
  rcu_process_callbacks();
}

/**
 * call_rcu - Queue a callback for invocation after a grace period
 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head)) {
  head->func = func;
  head->next = NULL;

  irq_flags_t flags = local_irq_save();
  struct rcu_data *rdp = this_cpu_ptr(rcu_percpu_data);

  *rdp->callbacks_tail = head;
  rdp->callbacks_tail = &head->next;

  local_irq_restore(flags);
}

/**
 * synchronize_rcu - Wait for a grace period to complete
 */
void synchronize_rcu(void) {
  uint64_t wait_gp;

  irq_flags_t flags = spinlock_lock_irqsave(&rcu_state.lock);

  /* Start a new GP if none in progress or current is already satisfied */
  rcu_start_gp();
  wait_gp = rcu_state.cur_gp;

  spinlock_unlock_irqrestore(&rcu_state.lock, flags);

  /* Current CPU is effectively in a QS while blocking */
  rcu_qs();

  /* Wait for the GP to complete */
  wait_event(&rcu_state.gp_wait, rcu_state.completed_gp >= wait_gp);
}
