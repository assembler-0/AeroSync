/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/fair.c
 * @brief Completely Fair Scheduler (CFS) implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/tsc.h> /* For get_time_ns */
#include <aerosync/sched/sched.h>
#include <lib/math.h>
#include <lib/printk.h>
#include <linux/container_of.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <mm/vma.h>

#define NS_PER_MS 1000000ULL
#define SCHED_LATENCY (6 * NS_PER_MS)
#define SCHED_MIN_GRANULARITY_NS (750000ULL)
#define SCHED_WAKEUP_GRANULARITY_NS (1000000ULL)

/*
 * Calculate the ideal slice for a task.
 * slice = latency * (task_weight / total_weight)
 */
static uint64_t sched_slice(struct cfs_rq *cfs_rq, struct sched_entity *se) {
  uint64_t slice = SCHED_LATENCY;

  if (cfs_rq->nr_running > SCHED_LATENCY / SCHED_MIN_GRANULARITY_NS) {
    slice = (uint64_t)cfs_rq->nr_running * SCHED_MIN_GRANULARITY_NS;
  }

  if (cfs_rq->load.weight > 0) {
    unsigned __int128 prod =
        (unsigned __int128)slice * (unsigned __int128)se->load.weight;
    slice = (uint64_t)(prod / (unsigned __int128)cfs_rq->load.weight);
  }

  return slice;
}

/*
 * This table maps nice values (-20 to 19) to their corresponding load weights.
 */
const uint32_t prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

/*
 * Update the min_vruntime of the runqueue.
 */
static void update_min_vruntime(struct cfs_rq *cfs_rq) {
  uint64_t vruntime = cfs_rq->min_vruntime;

  if (cfs_rq->rb_leftmost) {
    struct sched_entity *se =
        rb_entry(cfs_rq->rb_leftmost, struct sched_entity, run_node);

    if (se->vruntime < vruntime)
      vruntime = se->vruntime;
  }

  /* Ensure min_vruntime only moves forward */
  if (vruntime > cfs_rq->min_vruntime) {
    cfs_rq->min_vruntime = vruntime;
  }
}

/*
 * Calculate delta_vruntime based on actual execution time and load weight.
 */
static uint64_t __calc_delta(uint64_t delta_exec_ns, unsigned long weight) {
  if (weight == 0)
    return delta_exec_ns;
  unsigned __int128 prod =
      (unsigned __int128)delta_exec_ns * (unsigned __int128)NICE_0_LOAD;
  return (uint64_t)(prod / (unsigned __int128)weight);
}

/*
 * Enqueue a task into the rb-tree and update rb_leftmost cache
 */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se) {
  struct rb_node **link = &cfs_rq->tasks_timeline.rb_node;
  struct rb_node *parent = NULL;
  struct sched_entity *entry;

  while (*link) {
    parent = *link;
    entry = rb_entry(parent, struct sched_entity, run_node);

    if (se->vruntime < entry->vruntime) {
      link = &parent->rb_left;
    } else {
      link = &parent->rb_right;
    }
  }

  rb_link_node(&se->run_node, parent, link);
  rb_insert_color(&se->run_node, &cfs_rq->tasks_timeline);

  cfs_rq->rb_leftmost = rb_first(&cfs_rq->tasks_timeline);
}

static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se) {
  rb_erase(&se->run_node, &cfs_rq->tasks_timeline);
  cfs_rq->rb_leftmost = rb_first(&cfs_rq->tasks_timeline);
}

/*
 * Update execution statistics for the current task
 */
static void update_curr_fair(struct rq *rq) {
  struct task_struct *curr = rq->curr;
  struct sched_entity *se = &curr->se;
  struct cfs_rq *cfs_rq = &rq->cfs;
  uint64_t now_ns = rq->clock_task;
  uint64_t delta_exec_ns;

  if (curr->sched_class != &fair_sched_class)
    return;

  if (now_ns < se->exec_start_ns) {
    delta_exec_ns = 0;
  } else {
    delta_exec_ns = now_ns - se->exec_start_ns;
  }

  se->sum_exec_runtime += delta_exec_ns;
  se->exec_start_ns = now_ns;

  cfs_rq->exec_clock += delta_exec_ns;

  /* Update vruntime */
  se->vruntime += __calc_delta(delta_exec_ns, se->load.weight);

  update_min_vruntime(cfs_rq);

  /* Update PELT load tracking */
  update_load_avg(rq, se, 0);
}

/*
 * Place a task into the timeline.
 */
static void place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
                         int initial) {
  uint64_t vruntime = cfs_rq->min_vruntime;

  if (initial) {
    se->vruntime = vruntime;
  } else {
    /* Waking task penalty/compensation */
    if (se->vruntime < vruntime)
      se->vruntime = vruntime;
  }
}

/*
 * Enqueue task - sched_class interface
 */
static void enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags) {
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct sched_entity *se = &p->se;

  if (se->on_rq)
    return;

  /*
   * If waking up, place the entity relative to min_vruntime.
   * We need logic to handle 'initial' placement better.
   */
  if (flags & ENQUEUE_WAKEUP) {
    place_entity(cfs_rq, se, 0); // Not initial if waking up
  } else if (flags & ENQUEUE_MOVE) {
    /* Denormalize vruntime after migration */
    se->vruntime += cfs_rq->min_vruntime;
  }

  update_curr_fair(rq);

  __enqueue_entity(cfs_rq, se);
  se->on_rq = 1;
  cfs_rq->nr_running++;
  cfs_rq->load.weight += se->load.weight;

  rq->nr_running++;

  /* Update PELT load tracking */
  update_load_avg(rq, se, ENQUEUE_WAKEUP);
}

/*
 * Dequeue task - sched_class interface
 */
static void dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags) {
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct sched_entity *se = &p->se;
  int task_is_curr = (rq->curr == p);

  if (!se->on_rq)
    return;

  update_curr_fair(rq);

  /* Normalize vruntime if migrating */
  if (flags & DEQUEUE_MOVE) {
      se->vruntime -= cfs_rq->min_vruntime;
  }

  /* Only remove from tree if it is NOT current (current is already removed) */
  if (!task_is_curr) {
      __dequeue_entity(cfs_rq, se);
  }

  se->on_rq = 0;
  cfs_rq->nr_running--;
  cfs_rq->load.weight -= se->load.weight;

  rq->nr_running--;

  update_load_avg(rq, se, 0);
}

/*
 * Pick next task - sched_class interface
 */
static struct task_struct *pick_next_task_fair(struct rq *rq) {
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct rb_node *left = cfs_rq->rb_leftmost;

  if (!left)
    return NULL;

  struct sched_entity *se = rb_entry(left, struct sched_entity, run_node);
  struct task_struct *p = container_of(se, struct task_struct, se);

  if (!p)
    return NULL;

  /* Remove from tree to mark as running */
  __dequeue_entity(cfs_rq, se);

  return p;
}

/*
 * Put prev task - sched_class interface
 */
static void put_prev_task_fair(struct rq *rq, struct task_struct *prev) {
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct sched_entity *se = &prev->se;

  if (prev->state == TASK_RUNNING) {
    update_curr_fair(rq);
    /* Put back into tree if still runnable */
    __enqueue_entity(cfs_rq, se);
  }
}

/*
 * Set next task - sched_class interface
 */
static void set_next_task_fair(struct rq *rq, struct task_struct *p,
                               bool first) {
  struct sched_entity *se = &p->se;
  se->exec_start_ns = rq->clock_task;
  se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

/*
 * Task tick - sched_class interface
 */
static void task_tick_fair(struct rq *rq, struct task_struct *curr,
                           int queued) {
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct sched_entity *se = &curr->se;

  update_curr_fair(rq);

  if (cfs_rq->nr_running > 1) {
    uint64_t slice = sched_slice(cfs_rq, se);
    uint64_t delta_exec = se->sum_exec_runtime - se->prev_sum_exec_runtime;

    if (delta_exec > slice) {
      set_need_resched();
    }
  }
}

static void task_fork_fair(struct task_struct *p) {
  struct cfs_rq *cfs_rq = &this_rq()->cfs;
  struct sched_entity *se = &p->se;

  se->vruntime = cfs_rq->min_vruntime;
  se->sum_exec_runtime = 0;
  se->prev_sum_exec_runtime = 0;
  se->exec_start_ns = 0;
}

static void task_dead_fair(struct task_struct *p) {}

static void switched_from_fair(struct rq *rq, struct task_struct *p) {}

static void switched_to_fair(struct rq *rq, struct task_struct *p) {
  p->se.vruntime = rq->cfs.min_vruntime;
}

static void prio_changed_fair(struct rq *rq, struct task_struct *p,
                              int oldprio) {
  if (prio_less(p->prio, oldprio) && rq->curr == p) {
    set_need_resched();
  }
}

/*
 * Yield task - sched_class interface
 */
static void yield_task_fair(struct rq *rq) {
  struct task_struct *curr = rq->curr;
  struct cfs_rq *cfs_rq = &rq->cfs;
  struct sched_entity *se = &curr->se;

  /*
   * Simple yield implementation: move vruntime forward
   * We need to put it back in the tree (via put_prev) and re-pick,
   * but usually schedule() handles the put/pick.
   * Just updating vruntime ensures it moves to the right.
   */
  se->vruntime += sched_slice(cfs_rq, se);
}

/*
 * Check preemption - sched_class interface
 */
static void check_preempt_curr_fair(struct rq *rq, struct task_struct *p,
                                    int flags) {
  struct task_struct *curr = rq->curr;
  struct sched_entity *se = &curr->se;
  struct sched_entity *pse = &p->se;

  if (curr->sched_class != &fair_sched_class)
    return;

  if (se->vruntime > pse->vruntime + SCHED_WAKEUP_GRANULARITY_NS) {
    set_need_resched();
  }
}

/*
 * select_idle_sibling - Find an idle CPU in the same LLC or NUMA domain
 * @p: Task to place
 * @prev_cpu: Previous CPU the task ran on
 * @target: Target CPU (usually the waker)
 */
static int select_idle_sibling(struct task_struct *p, int prev_cpu, int target) {
  struct sched_domain *sd;
  struct rq *target_rq = per_cpu_ptr(runqueues, target);
  int best_cpu = target;
  unsigned long min_load = ULONG_MAX;

  /* 1. If target (waker) is idle, prioritize it (cache locality with waker) */
  if (target_rq->nr_running == 0)
    return target;

  /* 2. If prev_cpu is idle, prioritize it (cache locality for task) */
  if (prev_cpu != target) {
    struct rq *prev_rq = per_cpu_ptr(runqueues, prev_cpu);
    if (prev_rq->nr_running == 0)
      return prev_cpu;
  }

  /* 3. Scan the MC (Multi-Core / LLC) domain for an idle core */
  /* Start from target's domain */
  sd = target_rq->sd;

  /* Find MC domain (or whatever is the highest shared cache domain) */
  /* In our topology: SMT -> MC. We want MC. */
  while (sd && !(sd->flags & SD_SHARE_PKG_RESOURCES))
      sd = sd->parent;

  if (!sd)
      return target;

  /* Search for idle CPU in this domain */
  int cpu;
  for_each_cpu(cpu, &sd->span) {
      if (!cpumask_test_cpu(cpu, &p->cpus_allowed))
          continue;

      if (cpu == target || cpu == prev_cpu)
          continue;

      struct rq *rq = per_cpu_ptr(runqueues, cpu);

      /* Found an idle CPU? */
      if (rq->nr_running == 0) {
          return cpu;
      }

      /* Track least loaded CPU as fallback */
      /* Use load_avg from PELT for better decision */
      if (rq->cfs.avg.load_avg < min_load) {
          min_load = rq->cfs.avg.load_avg;
          best_cpu = cpu;
      }
  }

  /*
   * Fallback: If no idle CPU, return best_cpu (least loaded).
   */
  return best_cpu;
}

static int select_task_rq_fair(struct task_struct *p, int cpu, int wake_flags) {
  /*
   * If the task is pinned, we have no choice.
   */
  if (p->nr_cpus_allowed == 1)
    return cpumask_first(&p->cpus_allowed);

  /*
   * For wakeups, try to find an idle sibling to reduce latency.
   * 'cpu' passed here is usually the waker's CPU.
   */
  if (wake_flags & ENQUEUE_WAKEUP) {
    int new_cpu = select_idle_sibling(p, p->cpu, cpu);
    if (cpumask_test_cpu(new_cpu, &p->cpus_allowed))
      return new_cpu;
  }

  /* Fallback: Stick to previous CPU if allowed */
  if (cpumask_test_cpu(p->cpu, &p->cpus_allowed))
    return p->cpu;

  return cpumask_first(&p->cpus_allowed);
}

/*
 * Definition of the Fair Scheduling Class
 */
const struct sched_class fair_sched_class = {
    .next = &idle_sched_class,

    .enqueue_task = enqueue_task_fair,
    .dequeue_task = dequeue_task_fair,
    .yield_task = yield_task_fair,
    .check_preempt_curr = check_preempt_curr_fair,

    .pick_next_task = pick_next_task_fair,
    .put_prev_task = put_prev_task_fair,
    .set_next_task = set_next_task_fair,

    .task_tick = task_tick_fair,
    .task_fork = task_fork_fair,
    .task_dead = task_dead_fair,

    .switched_from = switched_from_fair,
    .switched_to = switched_to_fair,
    .prio_changed = prio_changed_fair,

    .update_curr = update_curr_fair,

    .select_task_rq = select_task_rq_fair,
};