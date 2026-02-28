/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/core.c
 * @brief Core scheduler implementation
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

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/fpu.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/tsc.h>
#include <drivers/apic/apic.h>
#include <aerosync/classes.h>
#include <aerosync/export.h>
#include <aerosync/sched/cpumask.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/mutex.h>
#include <aerosync/softirq.h>
#include <aerosync/signal.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <aerosync/timer.h>
#include <aerosync/resdomain.h>
#include <arch/x86_64/gdt/gdt.h>
#include <linux/rculist.h>
#include <aerosync/errno.h>

DEFINE_PER_CPU(int, __preempt_count) = 0;
DEFINE_PER_CPU(int, need_resched) = 0;

static int idle_balance(struct rq *this_rq);

/*
 * Scheduler Core Implementation
 */

/* Per-CPU runqueue */
DEFINE_PER_CPU(struct rq, runqueues);

/* Current task per CPU (cached for speed, though rq->curr exists) */
DEFINE_PER_CPU(struct task_struct *, current_task);

/* Idle task per CPU */
DEFINE_PER_CPU(struct task_struct, idle_task);

/* Preemption flag per CPU */
DEFINE_PER_CPU(int, need_resched);

void set_need_resched(void) { this_cpu_write(need_resched, 1); }

/*
 * Runqueue locking
 */
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

/*
 * Basic Helpers
 */

struct rq *this_rq(void) { return this_cpu_ptr(runqueues); }
EXPORT_SYMBOL(this_rq);
struct task_struct *get_current(void) { return this_cpu_read(current_task); }
EXPORT_SYMBOL(get_current);
void set_current(struct task_struct *t) { this_cpu_write(current_task, t); }
EXPORT_SYMBOL(set_current);
void set_task_cpu(struct task_struct *task, int cpu) { task->cpu = cpu; }
EXPORT_SYMBOL(set_task_cpu);

/*
 * Core Scheduler Operations
 */

void __no_cfi activate_task(struct rq *rq, struct task_struct *p, int flags) {
  if (p->sched_class && p->sched_class->enqueue_task) {
    p->sched_class->enqueue_task(rq, p, flags);
  }
}

void __no_cfi deactivate_task(struct rq *rq, struct task_struct *p, int flags) {
  if (p->sched_class && p->sched_class->dequeue_task) {
    p->sched_class->dequeue_task(rq, p, flags);
  }
}

/*
 * Internal migration helper - caller must hold __rq_lock
 */
static void __no_cfi __move_task_to_rq_locked(struct task_struct *task, int dest_cpu) {
  struct rq *src_rq = per_cpu_ptr(runqueues, task->cpu);
  struct rq *dest_rq = per_cpu_ptr(runqueues, dest_cpu);

  deactivate_task(src_rq, task, DEQUEUE_MOVE);
  set_task_cpu(task, dest_cpu);
  activate_task(dest_rq, task, ENQUEUE_MOVE);

  /* Update affinity cache or stats if needed */
  if (task->sched_class->migrate_task_rq) {
    task->sched_class->migrate_task_rq(task, dest_cpu);
  }
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

  /* Check affinity */
  if (!cpumask_test_cpu(dest_cpu, &task->cpus_allowed)) {
    /* warning or fail? */
    /* For force migration we might ignore, but generally we should respect */
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

/*
 * sched_move_task - Moves a task from its current scheduling group to a new one.
 * Used when a task is attached to a different ResDomain.
 */
void __no_cfi sched_move_task(struct task_struct *p) {
  struct rq *rq = per_cpu_ptr(runqueues, p->cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  bool queued = (p->se.on_rq != 0);
  if (queued) {
    deactivate_task(rq, p, DEQUEUE_SAVE);
  }

  /* 
   * This is where we update the hierarchy linkage.
   * We use the CPU controller state from the ResDomain.
   */
  struct cpu_rd_state *cs = p->rd ? (struct cpu_rd_state *)p->rd->subsys[RD_SUBSYS_CPU] : nullptr;
  if (cs && cs->cfs_rq) {
      p->se.cfs_rq = cs->cfs_rq[p->cpu];
      p->se.parent = cs->se ? cs->se[p->cpu] : nullptr;
  } else {
      p->se.cfs_rq = &rq->cfs;
      p->se.parent = nullptr;
  }

  if (queued) {
    activate_task(rq, p, ENQUEUE_RESTORE);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

EXPORT_SYMBOL(sched_move_task);

static void switch_mm(struct mm_struct *prev, struct mm_struct *next,
                      struct task_struct *tsk) {
  int cpu = smp_get_id();
  if (prev == next)
    return;

  if (prev) {
    cpumask_clear_cpu(cpu, &prev->cpu_mask);
  }

  if (next && next->pml_root) {
    cpumask_set_cpu(cpu, &next->cpu_mask);
    vmm_switch_pml_root((uint64_t) next->pml_root);
  } else {
    vmm_switch_pml_root(g_kernel_pml_root);
  }
}

/*
 * Pick the next task to run by iterating through scheduler classes
 */
static struct task_struct * __no_cfi pick_next_task(struct rq *rq) {
  struct task_struct *p;
  const struct sched_class *class;

  for_each_class(class) {
    p = class->pick_next_task(rq);
    if (p) {
      /* Notify class that we picked this task */
      if (class->set_next_task) {
        class->set_next_task(
          rq, p, true); /* true = first time picking in this cycle? */
        /* Note: Linux uses set_next_task slightly differently */
      }
      return p;
    }
  }

  /* Failure to pick any task (shouldn't happen if idle class exists) */
  return nullptr;
}

/*
 * Context Switch
 */
extern struct task_struct * __no_cfi switch_to(struct task_struct *prev,
                                     struct task_struct *next);

/*
 * Priority Inheritance Helpers
 */
extern void init_dl_rq(struct dl_rq *dl_rq);

/**
 * task_top_pi_prio - return the highest priority among PI waiters
 */
int task_top_pi_prio(struct task_struct *p) {
  if (list_empty(&p->pi_waiters))
    return p->normal_prio;

  struct task_struct *top_waiter =
      list_first_entry(&p->pi_waiters, struct task_struct, pi_list);
  return top_waiter->prio;
}

static inline int task_on_rq(struct task_struct *p) {
  if (p->sched_class == &fair_sched_class) return p->se.on_rq;
  if (p->sched_class == &rt_sched_class) return p->rt.on_rq;
  if (p->sched_class == &dl_sched_class) return p->dl.on_rq;
  return 0;
}

/**
 * __update_task_prio - update the effective priority of a task
 * Must be called with p->pi_lock held.
 */
void __no_cfi __update_task_prio(struct task_struct *p) {
  int old_prio = p->prio;
  int top_pi = task_top_pi_prio(p);
  const struct sched_class *old_class = p->sched_class;

  int new_prio = prio_less(p->normal_prio, top_pi) ? p->normal_prio : top_pi;

  if (old_prio == new_prio && old_class->pick_next_task) {
    bool policy_matches = true;
    if (dl_prio(new_prio) && old_class != &dl_sched_class) policy_matches = false;
    else if (rt_prio(new_prio) && old_class != &rt_sched_class) policy_matches = false;

    if (policy_matches) return;
  }

  p->prio = new_prio;

  if (dl_prio(p->prio)) {
    p->sched_class = &dl_sched_class;
  } else if (rt_prio(p->prio)) {
    p->sched_class = &rt_sched_class;
  } else {
    p->sched_class = &fair_sched_class;
  }

  if (!dl_prio(p->prio) && !rt_prio(p->prio)) {
    if (task_has_dl_policy(p)) p->sched_class = &dl_sched_class;
    else if (task_has_rt_policy(p)) p->sched_class = &rt_sched_class;
  }

  if (old_prio != p->prio || old_class != p->sched_class) {
    struct rq *rq = per_cpu_ptr(runqueues, p->cpu);

    spinlock_lock(&rq->lock);

    int on_rq = task_on_rq(p);
    if (on_rq) deactivate_task(rq, p, DEQUEUE_SAVE);

    if (old_class != p->sched_class) {
      if (old_class->switched_from) old_class->switched_from(rq, p);
      if (p->sched_class->switched_to) p->sched_class->switched_to(rq, p);
    } else if (p->sched_class->prio_changed) {
      p->sched_class->prio_changed(rq, p, old_prio);
    }

    if (on_rq) activate_task(rq, p, ENQUEUE_RESTORE);

    spinlock_unlock(&rq->lock);

    /* Propagate if this task is also blocked on a mutex */
    if (p->pi_blocked_on && p->pi_blocked_on->owner) {
      pi_boost_prio(p->pi_blocked_on->owner, p);
    }
  }
}

void update_task_prio(struct task_struct *p) {
  irq_flags_t flags = spinlock_lock_irqsave(&p->pi_lock);
  __update_task_prio(p);
  spinlock_unlock_irqrestore(&p->pi_lock, flags);
}

/**
 * pi_boost_prio - boost the priority of the owner of a mutex
 */
void pi_boost_prio(struct task_struct *owner, struct task_struct *waiter) {
  irq_flags_t flags = spinlock_lock_irqsave(&owner->pi_lock);
  struct task_struct *pos;
  bool added = false;

  if (!list_empty(&waiter->pi_list)) {
    list_del_init(&waiter->pi_list);
  }

  list_for_each_entry(pos, &owner->pi_waiters, pi_list) {
    if (prio_less(waiter->prio, pos->prio)) {
      list_add_tail(&waiter->pi_list, &pos->pi_list);
      added = true;
      break;
    }
  }
  if (!added) {
    list_add_tail(&waiter->pi_list, &owner->pi_waiters);
  }

  __update_task_prio(owner);
  spinlock_unlock_irqrestore(&owner->pi_lock, flags);
}

/**
 * pi_restore_prio - restore the priority of the owner of a mutex
 */
void pi_restore_prio(struct task_struct *owner, struct task_struct *waiter) {
  irq_flags_t flags = spinlock_lock_irqsave(&owner->pi_lock);
  list_del_init(&waiter->pi_list);
  __update_task_prio(owner);
  spinlock_unlock_irqrestore(&owner->pi_lock, flags);
}

/*
 * Task state management functions
 */

void __no_cfi task_sleep(void) {
  struct task_struct *curr = get_current();
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  if (curr->state == TASK_RUNNING) {
    curr->state = TASK_INTERRUPTIBLE;
  }

  if (rq->curr == curr) {
    if (curr->sched_class->update_curr)
      curr->sched_class->update_curr(rq);
  }

  deactivate_task(rq, curr, DEQUEUE_SLEEP);

  spinlock_unlock_irqrestore(&rq->lock, flags);

  schedule();
}

static void schedule_timeout_handler(struct timer_list *timer) {
  struct task_struct *task = (struct task_struct *) timer->data;
  task_wake_up(task);
}

long schedule_timeout(uint64_t ns) {
  struct timer_list timer;
  uint64_t expire;

  if (ns == 0) {
    schedule();
    return 0;
  }

  expire = get_time_ns() + ns;

  timer_setup(&timer, schedule_timeout_handler, get_current());
  timer_add(&timer, expire);

  schedule();

  timer_del(&timer);

  long remaining = (long) (expire - get_time_ns());
  return remaining < 0 ? 0 : remaining;
}

void msleep(unsigned int msecs) {
  uint64_t timeout = (uint64_t) msecs * NSEC_PER_MSEC;
  
  while (timeout) {
    set_current_state(TASK_UNINTERRUPTIBLE);
    timeout = schedule_timeout(timeout);
  }
}
EXPORT_SYMBOL(msleep);

unsigned int msleep_interruptible(unsigned int msecs) {
  uint64_t timeout = (uint64_t) msecs * NSEC_PER_MSEC;
  
  while (timeout && !signal_pending(current)) {
    set_current_state(TASK_INTERRUPTIBLE);
    timeout = schedule_timeout(timeout);
  }
  
  return (unsigned int) (timeout / NSEC_PER_MSEC);
}
EXPORT_SYMBOL(msleep_interruptible);

void __no_cfi task_wake_up(struct task_struct *task) {
  int cpu = smp_get_id();
  int target_cpu;
  struct rq *rq;
  irq_flags_t flags;

  /*
   * 1. Lock the task's PI lock to serialize against concurrent wakeups
   * This is the canonical way to start a wakeup in production kernels.
   */
  flags = spinlock_lock_irqsave(&task->pi_lock);

  if (task->state == TASK_RUNNING) {
    spinlock_unlock_irqrestore(&task->pi_lock, flags);
    return;
  }

  /* 2. Select the best CPU for this task */
  if (task->sched_class && task->sched_class->select_task_rq) {
    target_cpu = task->sched_class->select_task_rq(task, cpu, ENQUEUE_WAKEUP);
  } else {
    target_cpu = task->cpu;
  }

  /* 3. Lock the target runqueue */
  rq = per_cpu_ptr(runqueues, target_cpu);
  spinlock_lock(&rq->lock);

  /* 4. Handle migration if needed */
  if (target_cpu != task->cpu) {
    /* Update task's CPU field under rq lock */
    task->cpu = target_cpu;
  }

  /* 5. Activate the task */
  task->state = TASK_RUNNING;
  activate_task(rq, task, ENQUEUE_WAKEUP);

  /* 6. Preemption check */
  if (rq->curr && rq->curr->sched_class->check_preempt_curr) {
    rq->curr->sched_class->check_preempt_curr(rq, task, ENQUEUE_WAKEUP);
  }

  /* 7. Release locks */
  spinlock_unlock(&rq->lock);
  spinlock_unlock_irqrestore(&task->pi_lock, flags);

  /* 8. If the task was woken on a remote CPU, send an IPI to reschedule it */
  if (target_cpu != cpu) {
    reschedule_cpu(target_cpu);
  }
}

void task_wake_up_all(void) {
  /* Simple implementation: wake everyone? No, usually wake_up on waitqueue.
     This global function might be legacy. Kept empty or stub. */
}

/*
 * schedule_tail - Finish the context switch.
 * This is called by every task after it is switched in.
 * For new tasks, it's called via the entry stub.
 */
void schedule_tail(struct task_struct *prev) {
  struct rq *rq = this_rq();

  /* Release the runqueue lock held since schedule() */
  spinlock_unlock(&rq->lock);

  /* Restore FPU state for the current task */
  if (current->thread.fpu) {
    if (current->thread.fpu_used) {
      fpu_restore(current->thread.fpu);
    } else {
      fpu_init_task(current->thread.fpu);
      fpu_restore(current->thread.fpu);
      current->thread.fpu_used = true;
    }
  }

  if (prev && (prev->state == TASK_DEAD || prev->state == TASK_ZOMBIE)) {
    free_task(prev);
  }
}

void __no_cfi set_task_nice(struct task_struct *p, int nice) {
  if (nice < MIN_NICE)
    nice = MIN_NICE;
  if (nice > MAX_NICE)
    nice = MAX_NICE;

  if (p->nice == nice)
    return;

  struct rq *rq = per_cpu_ptr(runqueues, p->cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  if (rq->curr == p && p->sched_class->update_curr) {
    p->sched_class->update_curr(rq);
  }

  // int on_rq = p->se.on_rq;
  /* Just check CFS on_rq for now, or need generic way */
  /* If task is queued, we should dequeue, update, enqueue */

  /* Generic: deactivate, update, activate */
  /* Warning: this might be expensive */
  int running = (p->state == TASK_RUNNING); /* Simplification */

  if (running)
    deactivate_task(rq, p, DEQUEUE_SAVE);

  p->nice = nice;
  p->static_prio = nice + NICE_TO_PRIO_OFFSET;
  p->se.load.weight = prio_to_weight[p->static_prio];

  if (p->sched_class->prio_changed)
    p->sched_class->prio_changed(
      rq, p, p->nice); /* passing new nice as old prio? no fix later */

  if (running)
    activate_task(rq, p, ENQUEUE_RESTORE);

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

/*
 * The main schedule function
 */
void __no_cfi schedule(void) {
  struct task_struct *prev_task, *next_task;
  struct rq *rq = this_rq();

  /* Preemption check */
  if (current->preempt_count > 0) {
    /* We can't schedule if preemption is disabled! */
    /* Unless we are panicking/oopsing? */
    return;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);
  prev_task = rq->curr;

  /* Update stats */
  rq->stats.nr_switches++;

  if (prev_task) {
    if (prev_task->sched_class->update_curr)
      prev_task->sched_class->update_curr(rq);

    /* Put previous task */
    if (prev_task->sched_class->put_prev_task)
      prev_task->sched_class->put_prev_task(rq, prev_task);
  }

  /* XNU-style Direct Handoff */
  if (prev_task && prev_task->direct_handoff) {
    next_task = prev_task->direct_handoff;
    prev_task->direct_handoff = nullptr;

    /* Verify successor is runnable on THIS CPU and valid */
    if (next_task->cpu == rq->cpu && next_task->state == TASK_RUNNING) {
      /* Directly switch MM if needed */
      goto switch_tasks;
    }
  }

  /* Pick next task */
  next_task = pick_next_task(rq);

  if (next_task == rq->idle && rq->nr_running == 0) {
    /* Release rq lock before idle_balance as it might take other rq locks */
    spinlock_unlock(&rq->lock);
    if (idle_balance(rq)) {
      spinlock_lock(&rq->lock);
      next_task = pick_next_task(rq);
    } else {
      spinlock_lock(&rq->lock);
    }
  }

  if (!next_task) {
    /* Should never happen as idle class always returns a task */
    unmet_cond_crit_else(!rq->idle) {
      next_task = rq->idle;
    }
  }

  /* Prepare next task */
  /* set_next_task called in pick_next_task */

switch_tasks:
  if (prev_task != next_task) {
    rq->curr = next_task;
    set_current(next_task);

    /* Switch MM */
    unmet_cond_crit(!prev_task);
    if (next_task->mm) {
      switch_mm(prev_task->active_mm, next_task->mm, next_task);
      next_task->active_mm = next_task->mm;
    } else {
      /* Kernel thread */
      next_task->active_mm = prev_task->active_mm;
      switch_mm(prev_task->active_mm, next_task->active_mm, next_task);
    }

    /* FPU handling - Eager switching for now to ensure correctness */
    if (prev_task->thread.fpu && prev_task->thread.fpu_used) {
      fpu_save(prev_task->thread.fpu);
    }

    /* Update TSS RSP0 for the next task (Ring 0 stack pointer) */
    if (next_task->stack) {
      set_tss_rsp0((uint64_t) ((uint8_t *) next_task->stack + (PAGE_SIZE * 4)));
    }

    prev_task = switch_to(prev_task, next_task);

    schedule_tail(prev_task);

    restore_irq_flags(flags);
    return;
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

void __noreturn idle_loop(void) {
  while (1) {
    check_preempt();
    cpu_hlt();
  }
}

/*
 * IPI and Load Balancing
 */

void reschedule_cpu(int cpu) {
  ic_send_ipi(*per_cpu_ptr(cpu_apic_id, cpu), IRQ_SCHED_IPI_VECTOR,
              APIC_DELIVERY_MODE_FIXED);
}

void irq_sched_ipi_handler(void) { this_cpu_write(need_resched, 1); }

static inline bool task_can_run_on(struct task_struct *p, int cpu) {
  return cpumask_test_cpu(cpu, &p->cpus_allowed);
}

/**
 * find_busiest_group - Find the group with highest load in a domain
 */
static struct sched_group *find_busiest_group(struct sched_domain *sd, int this_cpu) {
  struct sched_group *busiest = nullptr, *sg = sd->groups;
  unsigned long max_load = 0;

  if (!sg) return nullptr;

  do {
    if (cpumask_test_cpu(this_cpu, &sg->cpumask)) {
      sg = sg->next;
      continue;
    }

    unsigned long avg_load = 0;
    int cpu;
    for_each_cpu(cpu, &sg->cpumask) {
      avg_load += per_cpu_ptr(runqueues, cpu)->cfs.avg.load_avg;
    }

    if (avg_load > max_load) {
      max_load = avg_load;
      busiest = sg;
    }
    sg = sg->next;
  } while (sg != sd->groups);

  return busiest;
}

/**
 * find_busiest_queue - Find the busiest runqueue in a group
 */
static struct rq *find_busiest_queue(struct sched_group *group, int this_cpu) {
  struct rq *busiest = nullptr;
  unsigned long max_load = 0;
  int cpu;

  for_each_cpu(cpu, &group->cpumask) {
    if (cpu == this_cpu)
      continue;

    struct rq *rq = per_cpu_ptr(runqueues, cpu);
    unsigned long load = rq->cfs.avg.load_avg;

    /*
     * For hybrid systems, we might want to prioritize pulling from
     * certain core types if they are overloaded.
     */
    if (load > max_load && rq->nr_running > 1) {
      max_load = load;
      busiest = rq;
    }
  }

  return busiest;
}

#ifdef SCHED_AUTO_BALANCE
/**
 * load_balance - Hierarchical load balancing across domains
 */
static void load_balance(void) {
  int this_cpu = smp_get_id();
  struct rq *rq = this_rq();
  struct sched_domain *sd;
  int pulled = 0;

  /*
   * Staggered balancing: each CPU starts balancing at a slightly different
   * time relative to its ID to avoid mass lock contention.
   */
  for (sd = rq->sd; sd; sd = sd->parent) {
    if (!(sd->flags & SD_LOAD_BALANCE))
      continue;

    /* Check if it's time to balance this domain */
    if (rq->clock < sd->next_balance)
      continue;

    struct sched_group *group = find_busiest_group(sd, this_cpu);
    if (!group) {
      /* No busier group found, delay next attempt */
      sd->next_balance = rq->clock + sd->min_interval;
      continue;
    }

    struct rq *src_rq = find_busiest_queue(group, this_cpu);
    if (!src_rq) {
      sd->next_balance = rq->clock + sd->min_interval;
      continue;
    }

    /*
     * Threshold logic: Only pull if the remote load is significantly
     * higher than our local load, or if we are idle.
     */
    unsigned long max_load = src_rq->cfs.avg.load_avg;
    unsigned long this_load = rq->cfs.avg.load_avg;
    bool force_balance = false;

#ifdef CONFIG_SCHED_HYBRID
    /* Hybrid Asymmetric Packing: Pull towards high-capacity cores */
    if ((sd->flags & SD_ASYM_PACKING) && rq->cpu_capacity > src_rq->cpu_capacity) {
      if (rq->nr_running < src_rq->nr_running)
        force_balance = true;
    }
#endif

    if (force_balance || max_load > this_load + (this_load / 4) + NICE_0_LOAD) {
      irq_flags_t flags = save_irq_flags();
      cpu_cli();
      double_rq_lock(src_rq, rq);

      /* Equalize load by moving tasks */
      unsigned long imbalance = (max_load - this_load) / 2;
      unsigned long moved_load = 0;
      int loop_limit = 32;

      struct rb_node *n = rb_first(&src_rq->cfs.tasks_timeline);
      while (n && moved_load < imbalance && loop_limit-- > 0) {
        struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
        struct task_struct *t = container_of(se, struct task_struct, se);

        n = rb_next(n);

        if (t == src_rq->curr) continue;
        if (!task_can_run_on(t, this_cpu)) continue;

        /* Move the task to our runqueue */
        __move_task_to_rq_locked(t, this_cpu);
        moved_load += se->load.weight;
        pulled++;

        /* Stop if we've pulled enough to satisfy imbalance */
        if (pulled >= 4 && moved_load >= imbalance) break;
      }

      if (pulled) {
        rq->stats.nr_load_balance++;
        rq->stats.nr_migrations += pulled;
      }

      double_rq_unlock(src_rq, rq);
      restore_irq_flags(flags);
    }

    /* Update timing for this domain */
    unsigned long interval = pulled ? sd->min_interval : sd->max_interval;
    sd->next_balance = rq->clock + interval;

    /* If we pulled tasks, we might not need to check higher domains this time */
    if (pulled > 2) break;
  }
}
#endif

/**
 * idle_balance - Attempt to pull tasks from other CPUs when becoming idle
 */
static int idle_balance(struct rq *this_rq) {
  struct sched_domain *sd;
  int pulled = 0;

  /* We can only balance if we're actually idle (nr_running == 0) */
  if (this_rq->nr_running > 0)
    return 0;

  for (sd = this_rq->sd; sd; sd = sd->parent) {
    if (!(sd->flags & SD_BALANCE_NEWIDLE))
      continue;

    struct sched_group *group = find_busiest_group(sd, this_rq->cpu);
    if (!group) continue;

    /* Find busiest CPU in that group */
    int busiest_cpu = -1;
    unsigned long max_load = 0;
    int cpu;
    for_each_cpu(cpu, &group->cpumask) {
      if (per_cpu_ptr(runqueues, cpu)->cfs.avg.load_avg > max_load) {
        max_load = per_cpu_ptr(runqueues, cpu)->cfs.avg.load_avg;
        busiest_cpu = cpu;
      }
    }

    if (busiest_cpu != -1 && max_load > NICE_0_LOAD) {
      struct rq *src_rq = per_cpu_ptr(runqueues, busiest_cpu);

      /* We don't hold src_rq->lock here yet, so we must be careful.
         Actually, load_balance logic uses double_rq_lock.
      */
      double_rq_lock(src_rq, this_rq);

      /* Find candidate tasks - move up to half load or until we have some work */
      unsigned long imbalance = (max_load - this_rq->cfs.avg.load_avg) / 2;
      unsigned long moved_load = 0;
      struct rb_node *n = rb_first(&src_rq->cfs.tasks_timeline);

      while (n && (moved_load < imbalance || pulled == 0)) {
        struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
        struct task_struct *t = container_of(se, struct task_struct, se);

        n = rb_next(n);

        if (t == src_rq->curr) continue;
        if (!task_can_run_on(t, this_rq->cpu)) continue;

        __move_task_to_rq_locked(t, this_rq->cpu);
        moved_load += se->load.weight;
        this_rq->stats.nr_migrations++;
        pulled++;

        /* For idle balance, even one task is enough to stop being idle, but grabbing a few is better for cache */
        if (pulled >= 2 && moved_load >= imbalance) break;
      }

      double_rq_unlock(src_rq, this_rq);
      if (pulled) break;
    }
  }

  return pulled;
}

#ifdef CONFIG_SCHED_AUTO_BALANCE
#ifdef CONFIG_SCHED_LB_PERIOD_MS
#define LOAD_BALANCE_INTERVAL_TICKS (CONFIG_SCHED_LB_PERIOD_MS / 10) /* Assuming 100Hz tick, so 10ms per tick */
#else
#define LOAD_BALANCE_INTERVAL_TICKS 100
#endif

static void run_rebalance_domains(struct softirq_action *h) {
  (void) h;
  load_balance();
}
#endif

void __hot __no_cfi scheduler_tick(void) {
  struct rq *rq = this_rq();
  struct task_struct *curr = rq->curr;

  spinlock_lock(&rq->lock);

  rq->clock++;
  rq->clock_task = get_time_ns(); // Update task clock

  if (curr && curr->sched_class->task_tick) {
    curr->sched_class->task_tick(rq, curr, 1 /* queued status? */);
  }

  spinlock_unlock(&rq->lock);

#ifdef CONFIG_SCHED_AUTO_BALANCE
  int cpu = smp_get_id();
#ifdef CONFIG_SCHED_TICK_STAGGER
  /* Stagger load balancing across CPUs to avoid synchronized lock contention */
  if ((rq->clock + cpu) % LOAD_BALANCE_INTERVAL_TICKS == 0) {
    raise_softirq(SCHED_SOFTIRQ);
  }
#else
  if (rq->clock % LOAD_BALANCE_INTERVAL_TICKS == 0) {
    raise_softirq(SCHED_SOFTIRQ);
  }
#endif
#endif

  rcu_check_callbacks();
}

void check_preempt(void) {
  if (this_cpu_read(need_resched) && preemptible()) {
    /* Clear flag executed in schedule? Or here?
       Linux clears it in entry assembly usually.
       Here we manual check. */
    this_cpu_write(need_resched, 0);
    schedule();
  }
}

static_assert(MAX_RT_PRIO == 100, "MAX_RT_PRIO != 100");

/*
 * Scheduler Initialization
 */
int sched_init(void) {
  pid_allocator_init();
  extern void kthread_init(void);
  kthread_init();

#ifdef SCHED_AUTO_BALANCE
  /* Register softirq for load balancing */
  open_softirq(SCHED_SOFTIRQ, run_rebalance_domains);
#endif
  
  for (int i = 0; i < MAX_CPUS; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    spinlock_init(&rq->lock);
    rq->cpu = i;
    rq->cpu_capacity = 1024; /* Default */

    /* Init CFS */
    rq->cfs.tasks_timeline = RB_ROOT;
    /* Init RT */
    // rt_rq_init(&rq->rt); // Need to expose this or do manually
    for (int j = 0; j < MAX_RT_PRIO_LEVELS; j++)
      INIT_LIST_HEAD(&rq->rt.queue[j]);
    rq->rt.rt_runtime = 950000000;
    /* Init Deadline */
    init_dl_rq(&rq->dl);
  }

  printk(SCHED_CLASS "CFS/RT/DL scheduler initialized for %d logical CPUs.\n",
         MAX_CPUS);

  /* Build topology domains */
  extern void build_sched_domains(void);
  build_sched_domains();
  return 0;
}

int sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  initial_task->mm = &init_mm;
  initial_task->active_mm = &init_mm;
  initial_task->node_id = cpu_to_node(initial_task->cpu);
  cpumask_set_cpu(smp_get_id(), &init_mm.cpu_mask);
  initial_task->state = TASK_RUNNING;
  initial_task->flags = PF_KTHREAD;
  initial_task->cpu = smp_get_id();
  initial_task->preempt_count = 0;

  /* Initial task is idle task for BSP essentially, until we spawn init */
  /* But we treat it as a normal task that becomes idle? */

  initial_task->sched_class = &fair_sched_class; /* Start as fair */
  initial_task->nice = 0;
  initial_task->static_prio = NICE_TO_PRIO_OFFSET + MAX_RT_PRIO;
  initial_task->normal_prio = initial_task->static_prio;
  initial_task->prio = initial_task->normal_prio;
  initial_task->rt_priority = 0;
  initial_task->se.load.weight = prio_to_weight[initial_task->nice + 20];
  initial_task->se.on_rq = 0;
  initial_task->se.exec_start_ns = get_time_ns();
  initial_task->se.cfs_rq = &rq->cfs;
  initial_task->se.parent = nullptr;
  
  extern struct pid_namespace init_pid_ns;
  initial_task->nsproxy = &init_pid_ns;

  /* PI initialization */
  spinlock_init(&initial_task->pi_lock);
  initial_task->pi_blocked_on = nullptr;
  INIT_LIST_HEAD(&initial_task->pi_waiters);
  INIT_LIST_HEAD(&initial_task->pi_list);

  cpumask_setall(&initial_task->cpus_allowed);

  /* Initialize files */
  extern struct files_struct init_files;
  initial_task->files = &init_files;

  /* Initialize task hierarchy lists for the boot task */
  INIT_LIST_HEAD(&initial_task->tasks);
  INIT_LIST_HEAD(&initial_task->children);
  INIT_LIST_HEAD(&initial_task->sibling);

  /* Add initial task to global task list */
  /* We don't add initial_task to the list because we switch to the per-cpu copy immediately */
  /*
  extern struct list_head task_list;
  extern spinlock_t tasklist_lock;
  irq_flags_t tflags = spinlock_lock_irqsave(&tasklist_lock);
  list_add_tail_rcu(&initial_task->tasks, &task_list);
  spinlock_unlock_irqrestore(&tasklist_lock, tflags);
  */

  /* Init per-cpu idle_task storage */
  struct task_struct *idle = this_cpu_ptr(idle_task);
  memcpy(idle, initial_task, sizeof(*idle));
  
  /* IMPORTANT: Re-initialize list heads after memcpy to avoid pointing to source lists */
  INIT_LIST_HEAD(&idle->tasks);
  INIT_LIST_HEAD(&idle->children);
  INIT_LIST_HEAD(&idle->sibling);
  INIT_LIST_HEAD(&idle->pi_waiters);
  INIT_LIST_HEAD(&idle->pi_list);

  /* Point rq->idle to the permanent storage */
  rq->idle = idle;
  rq->curr = idle;
  set_current(idle);
  return 0;
}

int sched_init_ap(void) {
  int cpu = smp_get_id();
  struct task_struct *idle = per_cpu_ptr(idle_task, cpu);

  memset(idle, 0, sizeof(*idle));
  snprintf(idle->comm, sizeof(idle->comm), "idle/%d", cpu);
  idle->cpu = cpu;
  idle->node_id = cpu_to_node(cpu);
  idle->flags = PF_KTHREAD | PF_IDLE;
  idle->state = TASK_RUNNING;
  idle->sched_class = &idle_sched_class;
  idle->static_prio = MAX_PRIO - 1;
  idle->normal_prio = idle->static_prio;
  idle->prio = idle->normal_prio;
  idle->preempt_count = 0;
  cpumask_set_cpu(cpu, &idle->cpus_allowed);

  /* PI initialization */
  spinlock_init(&idle->pi_lock);
  idle->pi_blocked_on = nullptr;
  INIT_LIST_HEAD(&idle->pi_waiters);
  INIT_LIST_HEAD(&idle->pi_list);

  INIT_LIST_HEAD(&idle->tasks);
  INIT_LIST_HEAD(&idle->children);
  INIT_LIST_HEAD(&idle->sibling);

  struct rq *rq = this_rq();

#ifdef CONFIG_SCHED_HYBRID
  struct cpuinfo_x86 *ci = this_cpu_ptr(cpu_info);
  if (ci->core_type == CORE_TYPE_INTEL_CORE) {
    rq->cpu_capacity = 1024;
  } else if (ci->core_type == CORE_TYPE_INTEL_ATOM) {
    rq->cpu_capacity = 512;
  } else {
    rq->cpu_capacity = 1024; /* Unknown */
  }
#else
  rq->cpu_capacity = 1024;
#endif

  rq->curr = idle;
  rq->idle = idle;
  idle->active_mm = &init_mm;
  cpumask_set_cpu(cpu, &init_mm.cpu_mask);
  set_current(idle);
  return 0;
}
