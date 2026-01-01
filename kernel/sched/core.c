/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sched/core.c
 * @brief Core scheduler implementation
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


#include <arch/x64/cpu.h>
#include <arch/x64/fpu.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/percpu.h>
#include <arch/x64/smp.h>
#include <arch/x64/tsc.h>      /* Added for get_time_ns */
#include <drivers/apic/apic.h> /* Added for IPI functions */
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/cpumask.h> /* Added for affinity */
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <kernel/sysintf/ic.h>
#include <kernel/wait.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <vsprintf.h>
#include <arch/x64/gdt/gdt.h>

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
int cpu_id(void) { return (int)smp_get_id(); }

struct rq *this_rq(void) { return this_cpu_ptr(runqueues); }

struct task_struct *get_current(void) { return this_cpu_read(current_task); }

void set_current(struct task_struct *t) { this_cpu_write(current_task, t); }

void set_task_cpu(struct task_struct *task, int cpu) { task->cpu = cpu; }

/*
 * Core Scheduler Operations
 */

void activate_task(struct rq *rq, struct task_struct *p) {
  if (p->sched_class && p->sched_class->enqueue_task) {
    p->sched_class->enqueue_task(rq, p, ENQUEUE_WAKEUP);
  }
}

void deactivate_task(struct rq *rq, struct task_struct *p) {
  if (p->sched_class && p->sched_class->dequeue_task) {
    p->sched_class->dequeue_task(rq, p, 0);
  }
}

/*
 * Internal migration helper - caller must hold __rq_lock
 */
static void __move_task_to_rq_locked(struct task_struct *task, int dest_cpu) {
  struct rq *src_rq = per_cpu_ptr(runqueues, task->cpu);
  struct rq *dest_rq = per_cpu_ptr(runqueues, dest_cpu);

  deactivate_task(src_rq, task);
  set_task_cpu(task, dest_cpu);
  activate_task(dest_rq, task);

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
  if (prev == next)
    return;

  if (next && next->pml4) {
    vmm_switch_pml4((uint64_t)next->pml4);
  } else {
    vmm_switch_pml4(g_kernel_pml4);
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
 * Task state management functions
 */

void task_sleep(void) {
  struct task_struct *curr = get_current();
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  /* If already running, we are going to sleep.
     If task state is RUNNING, user probably forgot to set state to SLEEPING?
     Wait, standard semantics: user sets state, then calls schedule.
     This helper forces sleep?
     Let's match existing behavior but be safe.
  */

  if (curr->state == TASK_RUNNING) {
    /* If called explicitly to sleep without setting state, assume
     * interruptible? */
    /* Actually, usually you set state then call schedule().
       This helper is "force sleep". */
    curr->state = TASK_INTERRUPTIBLE;
  }

  if (rq->curr == curr) {
    if (curr->sched_class->update_curr)
      curr->sched_class->update_curr(rq);
  }

  deactivate_task(rq, curr);

  spinlock_unlock_irqrestore(&rq->lock, flags);

  schedule();
}

void task_wake_up(struct task_struct *task) {
  struct rq *rq = per_cpu_ptr(runqueues, task->cpu);

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  if (task->state != TASK_RUNNING) {
    task->state = TASK_RUNNING;
    activate_task(rq, task);

    /* Preemption check */
    if (rq->curr && rq->curr->sched_class->check_preempt_curr) {
      rq->curr->sched_class->check_preempt_curr(rq, task, ENQUEUE_WAKEUP);
    }
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
    deactivate_task(rq, p);

  p->nice = nice;
  p->static_prio = nice + NICE_TO_PRIO_OFFSET;
  p->se.load.weight = prio_to_weight[p->static_prio];

  if (p->sched_class->prio_changed)
    p->sched_class->prio_changed(
        rq, p, p->nice); /* passing new nice as old prio? no fix later */

  if (running)
    activate_task(rq, p);

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
        set_tss_rsp0((uint64_t)((uint8_t *)next_task->stack + (PAGE_SIZE * 4)));
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

static void load_balance(void) {
  int this_cpu = smp_get_id();

  /* Optimization: Only CPU 0 balances? Or distributed?
     For now, to fix "wasteful", let's have only CPU 0 do it,
     or do it less frequently.
  */
  if (this_cpu != 0)
    return;

  unsigned long total_cpus = smp_get_cpu_count();
  if (total_cpus <= 1)
    return;

  int overloaded_cpu = -1;
  int underloaded_cpu = -1;
  unsigned long max_load = 0;
  unsigned long min_load = ~0UL;

  /* Basic O(N) balancer */
  for (unsigned int i = 0; i < total_cpus; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    unsigned long load =
        rq->avg_load; /* Note: only considers CFS load usually */
    /* Ideally we check nr_running too */

    if (load > max_load) {
      max_load = load;
      overloaded_cpu = (int)i;
    }
    if (load < min_load) {
      min_load = load;
      underloaded_cpu = (int)i;
    }
  }

  if (overloaded_cpu == -1 || underloaded_cpu == -1 ||
      (max_load - min_load <= NICE_0_LOAD)) {
    return;
  }

  struct rq *src_rq = per_cpu_ptr(runqueues, overloaded_cpu);
  struct rq *dst_rq = per_cpu_ptr(runqueues, underloaded_cpu);

  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  double_rq_lock(src_rq, dst_rq);

  if (src_rq->avg_load - dst_rq->avg_load <= NICE_0_LOAD) {
    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);
    return;
  }

  /* Find task to migrate */
  /* We need to peek into CFS structure which is internal to fair.c usually,
     but we have definitions in sched.h now. */
  struct rb_node *n = rb_first(&src_rq->cfs.tasks_timeline);
  struct task_struct *candidate = NULL;

  for (; n; n = rb_next(n)) {
    struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
    struct task_struct *t = container_of(se, struct task_struct, se);

    if (t == src_rq->curr)
      continue;
    if (!task_can_run_on(t, underloaded_cpu))
      continue;

    /* Found one */
    candidate = t;
    break;
  }

  if (candidate) {
    __move_task_to_rq_locked(candidate, underloaded_cpu);

    /* Stats */
    dst_rq->stats.nr_migrations++;
    dst_rq->stats.nr_load_balance++;

    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);

    reschedule_cpu(underloaded_cpu);
  } else {
    double_rq_unlock(src_rq, dst_rq);
    restore_irq_flags(flags);
  }
}

#define LOAD_BALANCE_INTERVAL_TICKS 100

void __hot scheduler_tick(void) {
  struct rq *rq = this_rq();
  struct task_struct *curr = rq->curr;

  spinlock_lock((volatile int *)&rq->lock);

  rq->clock++;
  rq->clock_task = get_time_ns(); // Update task clock

  if (curr && curr->sched_class->task_tick) {
    curr->sched_class->task_tick(rq, curr, 1 /* queued status? */);
  }

  if (rq->clock % LOAD_BALANCE_INTERVAL_TICKS == 0) {
    load_balance();
  }

  spinlock_unlock((volatile int *)&rq->lock);
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

  for (int i = 0; i < MAX_CPUS; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    spinlock_init(&rq->lock);
    rq->cpu = i;

    /* Init CFS */
    rq->cfs.tasks_timeline = RB_ROOT;
    /* Init RT */
    // rt_rq_init(&rq->rt); // Need to expose this or do manually
    for (int j = 0; j < MAX_RT_PRIO_LEVELS; j++)
      INIT_LIST_HEAD(&rq->rt.queue[j]);
    rq->rt.rt_runtime = 950000000;
  }

  printk(SCHED_CLASS "CFS/RT scheduler initialized for %d logical CPUs.\n",
         MAX_CPUS);
}

void sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  initial_task->mm = &init_mm;
  initial_task->active_mm = &init_mm;
  initial_task->state = TASK_RUNNING;
  initial_task->flags = PF_KTHREAD;
  initial_task->cpu = cpu_id();
  initial_task->preempt_count = 0;

  /* Initial task is idle task for BSP essentially, until we spawn init */
  /* But we treat it as a normal task that becomes idle? */

  initial_task->sched_class = &fair_sched_class; /* Start as fair */
  initial_task->nice = 0;
  initial_task->static_prio = NICE_TO_PRIO_OFFSET;
  initial_task->se.load.weight = prio_to_weight[initial_task->static_prio];
  initial_task->se.on_rq = 0;
  initial_task->se.exec_start_ns = get_time_ns();

  cpumask_setall(&initial_task->cpus_allowed);

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
  idle->flags = PF_KTHREAD | PF_IDLE;
  idle->state = TASK_RUNNING;
  idle->sched_class = &idle_sched_class;
  idle->preempt_count = 0;
  cpumask_set_cpu(cpu, &idle->cpus_allowed);

  INIT_LIST_HEAD(&idle->tasks);
  INIT_LIST_HEAD(&idle->children);
  INIT_LIST_HEAD(&idle->sibling);

  struct rq *rq = this_rq();
  rq->curr = idle;
  rq->idle = idle;
  set_current(idle);
}