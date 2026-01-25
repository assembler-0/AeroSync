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
#include <arch/x86_64/tsc.h>      /* Added for get_time_ns */
#include <drivers/apic/apic.h> /* Added for IPI functions */
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/sched/cpumask.h> /* Added for affinity */
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/mutex.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <aerosync/timer.h>
#include <vsprintf.h>
#include <arch/x86_64/gdt/gdt.h>

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
int cpu_id(void) { return (int) smp_get_id(); }

struct rq *this_rq(void) { return this_cpu_ptr(runqueues); }

struct task_struct *get_current(void) { return this_cpu_read(current_task); }

void set_current(struct task_struct *t) { this_cpu_write(current_task, t); }

void set_task_cpu(struct task_struct *task, int cpu) { task->cpu = cpu; }

/*
 * Core Scheduler Operations
 */

void activate_task(struct rq *rq, struct task_struct *p, int flags) {
  if (p->sched_class && p->sched_class->enqueue_task) {
    p->sched_class->enqueue_task(rq, p, flags);
  }
}

void deactivate_task(struct rq *rq, struct task_struct *p, int flags) {
  if (p->sched_class && p->sched_class->dequeue_task) {
    p->sched_class->dequeue_task(rq, p, flags);
  }
}

/*
 * Internal migration helper - caller must hold __rq_lock
 */
static void __move_task_to_rq_locked(struct task_struct *task, int dest_cpu) {
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

static void switch_mm(struct mm_struct *prev, struct mm_struct *next,
                      struct task_struct *tsk) {
  int cpu = cpu_id();
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
static struct task_struct *pick_next_task(struct rq *rq) {
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
  return NULL;
}

/*
 * Context Switch
 */
extern struct task_struct *switch_to(struct task_struct *prev,
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
void __update_task_prio(struct task_struct *p) {
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

void task_sleep(void) {
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
  struct task_struct *task = (struct task_struct *)timer->data;
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

  long remaining = (long)(expire - get_time_ns());
  return remaining < 0 ? 0 : remaining;
}

void task_wake_up(struct task_struct *task) {
  int cpu = cpu_id();
  int target_cpu = task->cpu;
  struct rq *rq;
  irq_flags_t flags;

  /* 
   * Select the best CPU for this task.
   * This allows the scheduler class to implement wake-up balancing (e.g., pulling to idle CPU).
   */
  if (task->sched_class && task->sched_class->select_task_rq) {
    target_cpu = task->sched_class->select_task_rq(task, cpu, ENQUEUE_WAKEUP);
  }

  /* 
   * Lock the PREVIOUS runqueue to serialize against concurrent wakeups 
   * or the task still being put to sleep.
   */
  struct rq *prev_rq = per_cpu_ptr(runqueues, task->cpu);
  flags = spinlock_lock_irqsave(&prev_rq->lock);

  if (task->state == TASK_RUNNING) {
    /* Already running, nothing to do */
    spinlock_unlock_irqrestore(&prev_rq->lock, flags);
    return;
  }

  /* Handle migration if the selected CPU is different */
  if (target_cpu != task->cpu) {
    set_task_cpu(task, target_cpu);
  }

  /* 
   * If target CPU is different from previous, we need to switch locks.
   * Note: We are holding prev_rq->lock.
   */
  rq = per_cpu_ptr(runqueues, target_cpu);
  if (rq != prev_rq) {
    spinlock_unlock(&prev_rq->lock);
    spinlock_lock(&rq->lock);
  }

  /* Now we hold rq->lock (target runqueue) */

  task->state = TASK_RUNNING;
  activate_task(rq, task, ENQUEUE_WAKEUP);

  /* Preemption check */
  if (rq->curr && rq->curr->sched_class->check_preempt_curr) {
    rq->curr->sched_class->check_preempt_curr(rq, task, ENQUEUE_WAKEUP);
  }

  /* 
   * If we woke up a task on a remote CPU, we might need to kick it 
   * so it reschedules immediately (if priority is higher).
   */
  if (target_cpu != cpu) {
     reschedule_cpu(target_cpu);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
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
  cpu_sti(); // Matches spinlock_lock_irqsave behavior

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

void set_task_nice(struct task_struct *p, int nice) {
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

  int on_rq =
      p->se.on_rq; /* Just check CFS on_rq for now, or need generic way */
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
void schedule(void) {
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
    if (rq->idle) {
      next_task = rq->idle;
    } else {
      panic("schedule(): No task to run and no idle task!");
    }
  }

  /* Prepare next task */
  /* set_next_task called in pick_next_task */

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
    return;
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
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
  struct sched_group *busiest = NULL, *sg = sd->groups;
  unsigned long max_load = 0;

  while (sg) {
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
  }

  return busiest;
}

/**
 * load_balance - Hierarchical load balancing across domains
 */
static void load_balance(void) {
  int this_cpu = smp_get_id();
  struct rq *rq = this_rq();
  struct sched_domain *sd;

  /* Traverse up the hierarchy (SMT -> MC -> NUMA) */
  for (sd = rq->sd; sd; sd = sd->parent) {
    if (!(sd->flags & SD_LOAD_BALANCE))
      continue;

    /* Check if it's time to balance this domain */
    if (rq->clock < sd->next_balance)
      continue;

    struct sched_group *group = find_busiest_group(sd, this_cpu);
    if (!group) {
      sd->next_balance = rq->clock + sd->min_interval;
      continue;
    }

    /* Find busiest CPU in that group */
    int busiest_cpu = -1;
    unsigned long max_load = 0;
    int cpu;
    for_each_cpu(cpu, &group->cpumask) {
      struct rq *remote_rq = per_cpu_ptr(runqueues, cpu);
      /* Use PELT load avg */
      unsigned long load = remote_rq->cfs.avg.load_avg;
      
      /* Also consider nr_running to avoid pulling from 1 task to 0 task if load is high? 
         If nr_running <= 1, usually don't pull unless we are 0?
      */
      
      if (load > max_load && remote_rq->nr_running > 1) {
        max_load = load;
        busiest_cpu = cpu;
      }
    }

    /* 
     * Threshold: Only pull if imbalance is significant.
     * OR if ASYM_PACKING is enabled and we are a higher-capacity CPU.
     */
    bool force_balance = false;
#ifdef CONFIG_SCHED_HYBRID
    if ((sd->flags & SD_ASYM_PACKING) && busiest_cpu != -1) {
        struct rq *src_rq = per_cpu_ptr(runqueues, busiest_cpu);
        if (rq->cpu_capacity > src_rq->cpu_capacity && rq->nr_running < src_rq->nr_running) {
            force_balance = true;
        }
    }
#endif

    if (busiest_cpu != -1 && (force_balance || max_load > rq->cfs.avg.load_avg + (rq->cfs.avg.load_avg / 4) + NICE_0_LOAD)) {
      struct rq *src_rq = per_cpu_ptr(runqueues, busiest_cpu);

      irq_flags_t flags = save_irq_flags();
      cpu_cli();
      double_rq_lock(src_rq, rq);

      /* Calculate how much load to move to equalize */
      unsigned long total_load = max_load + rq->cfs.avg.load_avg;
      unsigned long target_load = total_load / 2;
      unsigned long imbalance = max_load - target_load;
      
      unsigned long moved_load = 0;
      int moved_count = 0;

      /* Migration logic - try to move tasks until imbalance is met */
      struct rb_node *n = rb_first(&src_rq->cfs.tasks_timeline);
      
      /* Limit iterations to avoid holding lock too long */
      int loop_limit = 32;

      while (n && moved_load < imbalance && loop_limit-- > 0) {
        struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
        struct task_struct *t = container_of(se, struct task_struct, se);
        
        /* Save next node before potential removal */
        n = rb_next(n);

        /* Skip if task is running (current) - simpler not to move it */
        if (t == src_rq->curr) continue;
        
        /* IMPORTANT: Check affinity */
        if (!task_can_run_on(t, this_cpu)) continue;
        
        /* Don't move cache-hot tasks? We lack 'last_run' timestamp in task struct for now.
           Assuming task at left of tree (low vruntime) starved? No, low vruntime means it needs to run.
           Maybe moving leftmost is good for fairness.
        */

        unsigned long task_load = se->load.weight;
        
        /* Move the task */
        __move_task_to_rq_locked(t, this_cpu);
        
        moved_load += task_load;
        moved_count++;
        
        rq->stats.nr_migrations++;
      }
      
      if (moved_count > 0) {
        rq->stats.nr_load_balance++;
        /* 
         * Reduced verbosity: Only print if we moved a significant chunk or for debugging.
         */
        /* printk(KERN_DEBUG SCHED_CLASS "LB: Moved %d tasks from CPU%d to CPU%d\n", moved_count, busiest_cpu, this_cpu); */
      }

      double_rq_unlock(src_rq, rq);
      restore_irq_flags(flags);
    }

    sd->next_balance = rq->clock + sd->min_interval;
  }
}

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

#define LOAD_BALANCE_INTERVAL_TICKS 100

void __hot scheduler_tick(void) {
  struct rq *rq = this_rq();
  struct task_struct *curr = rq->curr;

  spinlock_lock((volatile int *) &rq->lock);

  rq->clock++;
  rq->clock_task = get_time_ns(); // Update task clock

  if (curr && curr->sched_class->task_tick) {
    curr->sched_class->task_tick(rq, curr, 1 /* queued status? */);
  }

  spinlock_unlock((volatile int *) &rq->lock);

  if (rq->clock % LOAD_BALANCE_INTERVAL_TICKS == 0) {
    load_balance();
  }

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

/*
 * Scheduler Initialization
 */
void sched_init(void) {
  pid_allocator_init();

  /* Sanity check */
  if (MAX_RT_PRIO != 100) {
    panic("sched_init: MAX_RT_PRIO must be 100 for current PI implementation");
  }

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
}

void sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  initial_task->mm = &init_mm;
  initial_task->active_mm = &init_mm;
  initial_task->node_id = cpu_to_node(initial_task->cpu);
  cpumask_set_cpu(cpu_id(), &init_mm.cpu_mask);
  initial_task->state = TASK_RUNNING;
  initial_task->flags = PF_KTHREAD;
  initial_task->cpu = cpu_id();
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

  /* PI initialization */
  spinlock_init(&initial_task->pi_lock);
  initial_task->pi_blocked_on = NULL;
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
  extern struct list_head task_list;
  list_add_tail(&initial_task->tasks, &task_list);

  rq->curr = initial_task;
  // rq->idle = initial_task; // Will be set in sched_init_ap for APs, logic for
  // BSP?
  /* BSP idle task needs to be distinct from init task usually.
     Here "initial_task" is the one running at boot.
     We usually fork init from it. Then this task becomes idle loop task.
  */
  rq->idle = initial_task;
  set_current(initial_task);

  /* Init per-cpu idle_task storage */
  struct task_struct *idle = this_cpu_ptr(idle_task);
  memcpy(idle, initial_task, sizeof(*idle));
  /* Point rq->idle to the permanent storage */
  rq->idle = idle;
}

void sched_init_ap(void) {
  int cpu = cpu_id();
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
  idle->pi_blocked_on = NULL;
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
    rq->cpu_capacity = 1024;
  }
#else
  rq->cpu_capacity = 1024;
#endif

  rq->curr = idle;
  rq->idle = idle;
  idle->active_mm = &init_mm;
  cpumask_set_cpu(cpu, &init_mm.cpu_mask);
  set_current(idle);
}
