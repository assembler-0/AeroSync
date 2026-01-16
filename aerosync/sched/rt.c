/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/rt.c
 * @brief Real-Time Scheduler (SCHED_FIFO, SCHED_RR) implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * Implements the Real-Time scheduling class with support for:
 * - SCHED_FIFO: First-In-First-Out real-time scheduling
 * - SCHED_RR: Round-Robin real-time scheduling
 *
 * RT tasks have higher priority than CFS tasks and lower than Deadline tasks.
 */

#include <arch/x86_64/tsc.h>
#include <aerosync/classes.h>
#include <aerosync/sched/sched.h>
#include <lib/printk.h>
#include <linux/container_of.h>

#define RT_CLASS "[sched/rt]: "

/* RT bandwidth defaults */
#define RT_RUNTIME_DEFAULT (950 * NSEC_PER_MSEC) /* 950ms per second */
#define RT_PERIOD_DEFAULT NSEC_PER_SEC           /* 1 second period */

/* Bitmap operations for priority queue */
static inline void __set_bit(int nr, uint64_t *bitmap) {
  bitmap[nr / 64] |= (1ULL << (nr % 64));
}

static inline void __clear_bit(int nr, uint64_t *bitmap) {
  bitmap[nr / 64] &= ~(1ULL << (nr % 64));
}

static inline int __test_bit(int nr, const uint64_t *bitmap) {
  return (bitmap[nr / 64] >> (nr % 64)) & 1;
}

/**
 * Find first set bit in bitmap (lowest priority number = highest priority)
 */
static inline int sched_find_first_bit(const uint64_t *bitmap) {
  for (int i = 0; i < 2; i++) {
    if (bitmap[i]) {
      return i * 64 + __builtin_ctzll(bitmap[i]);
    }
  }
  return MAX_RT_PRIO_LEVELS;
}

/**
 * rt_rq_init - Initialize RT runqueue
 * @rt_rq: RT runqueue to initialize
 */
static void rt_rq_init(struct rt_rq *rt_rq) {
  for (int i = 0; i < MAX_RT_PRIO_LEVELS; i++) {
    INIT_LIST_HEAD(&rt_rq->queue[i]);
  }
  rt_rq->bitmap[0] = 0;
  rt_rq->bitmap[1] = 0;
  rt_rq->rt_nr_running = 0;
  rt_rq->rt_time = 0;
  rt_rq->rt_runtime = RT_RUNTIME_DEFAULT;
  rt_rq->rt_throttled = 0;
  spinlock_init(&rt_rq->lock);
}

/**
 * enqueue_task_rt - Add RT task to runqueue
 */
void enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags) {
  struct sched_rt_entity *rt_se = &p->rt;
  struct rt_rq *rt_rq = &rq->rt;
  int prio = p->prio;

  if (rt_se->on_rq)
    return;

  /* For RT class, prio must be < 100 */
  if (prio >= MAX_RT_PRIO_LEVELS) prio = MAX_RT_PRIO_LEVELS - 1;

  /* Add to priority queue */
  list_add_tail(&rt_se->run_list, &rt_rq->queue[prio]);
  __set_bit(prio, rt_rq->bitmap);
  rt_se->on_rq = 1;
  rt_rq->rt_nr_running++;
  rq->nr_running++;

  /* Initialize time slice for SCHED_RR */
  if (p->policy == SCHED_RR && rt_se->time_slice == 0) {
    rt_se->time_slice = RR_TIMESLICE / NSEC_PER_MSEC;
  }
}

/**
 * dequeue_task_rt - Remove RT task from runqueue
 */
void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags) {
  struct sched_rt_entity *rt_se = &p->rt;
  struct rt_rq *rt_rq = &rq->rt;
  int prio = p->prio;

  if (!rt_se->on_rq)
    return;

  if (prio >= MAX_RT_PRIO_LEVELS) prio = MAX_RT_PRIO_LEVELS - 1;

  list_del(&rt_se->run_list);
  rt_se->on_rq = 0;
  rt_rq->rt_nr_running--;
  rq->nr_running--;

  /* Clear bitmap bit if queue is now empty */
  if (list_empty(&rt_rq->queue[prio])) {
    __clear_bit(prio, rt_rq->bitmap);
  }
}

/**
 * yield_task_rt - Handle sched_yield() for RT task
 */
static void yield_task_rt(struct rq *rq) {
  struct task_struct *curr = rq->curr;
  struct sched_rt_entity *rt_se = &curr->rt;
  struct rt_rq *rt_rq = &rq->rt;
  int prio = curr->prio;

  if (!rt_se->on_rq)
    return;

  if (prio >= MAX_RT_PRIO_LEVELS) prio = MAX_RT_PRIO_LEVELS - 1;

  /* Move to end of same priority queue */
  list_del(&rt_se->run_list);
  list_add_tail(&rt_se->run_list, &rt_rq->queue[prio]);

  /* Reset time slice for SCHED_RR */
  if (curr->policy == SCHED_RR) {
    rt_se->time_slice = RR_TIMESLICE / NSEC_PER_MSEC;
  }
}

/**
 * check_preempt_curr_rt - Check if waking RT task should preempt current
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p,
                                  int flags) {
  struct task_struct *curr = rq->curr;

  /* RT always preempts non-RT */
  if (curr->sched_class != &rt_sched_class) {
    set_need_resched();
    return;
  }

  /* Higher priority (lower number) RT task preempts */
  if (p->prio < curr->prio) {
    set_need_resched();
  }
}

/**
 * pick_next_task_rt - Select highest priority RT task
 */
struct task_struct *pick_next_task_rt(struct rq *rq) {
  struct rt_rq *rt_rq = &rq->rt;
  struct sched_rt_entity *rt_se;
  struct task_struct *p;
  int idx;

  if (rt_rq->rt_nr_running == 0)
    return NULL;

  /* Find highest priority (lowest index) with runnable tasks */
  idx = sched_find_first_bit(rt_rq->bitmap);
  if (idx >= MAX_RT_PRIO_LEVELS)
    return NULL;

  /* Get first task from that priority level */
  rt_se =
      list_first_entry(&rt_rq->queue[idx], struct sched_rt_entity, run_list);
  p = container_of(rt_se, struct task_struct, rt);

  /* Dequeue from wait list (will run) */
  dequeue_task_rt(rq, p, 0);

  return p;
}

/**
 * put_prev_task_rt - Called when RT task is being switched out
 */
void put_prev_task_rt(struct rq *rq, struct task_struct *p) {
  struct sched_rt_entity *rt_se = &p->rt;

  /* Re-enqueue if still runnable */
  if (p->state == TASK_RUNNING && !rt_se->on_rq) {
    enqueue_task_rt(rq, p, 0);
  }
}

/**
 * set_next_task_rt - Prepare RT task to run
 */
static void set_next_task_rt(struct rq *rq, struct task_struct *p, bool first) {
  /* Update exec start time */
  /* Nothing special needed for RT */
}

/**
 * task_tick_rt - Timer tick for running RT task
 */
void task_tick_rt(struct rq *rq, struct task_struct *p, int queued) {
  struct sched_rt_entity *rt_se = &p->rt;
  struct rt_rq *rt_rq = &rq->rt;
  int prio = p->prio;

  if (prio >= MAX_RT_PRIO_LEVELS) prio = MAX_RT_PRIO_LEVELS - 1;

  /* SCHED_FIFO tasks don't have time slices */
  if (p->policy == SCHED_FIFO)
    return;

  /* SCHED_RR: decrement time slice */
  if (p->policy == SCHED_RR) {
    if (rt_se->time_slice > 0) {
      rt_se->time_slice--;
    }

    if (rt_se->time_slice == 0) {
      /* Time slice expired, move to end of queue */
      rt_se->time_slice = RR_TIMESLICE / NSEC_PER_MSEC;

      /* Only reschedule if there are other tasks at same priority */
      if (rt_rq->rt_nr_running > 1 &&
          !list_empty(&rt_rq->queue[prio]) &&
          list_is_singular(&rt_rq->queue[prio]) == 0) {
        set_need_resched();
      }
    }
  }

  /* Track RT bandwidth usage */
  rt_rq->rt_time++;
}

/**
 * task_fork_rt - Called when RT task forks
 */
static void task_fork_rt(struct task_struct *p) {
  /* Child inherits parent's RT priority */
  p->rt.time_slice = RR_TIMESLICE / NSEC_PER_MSEC;
}

/**
 * task_dead_rt - Called when RT task exits
 */
static void task_dead_rt(struct task_struct *p) { /* Nothing special for RT */ }

/**
 * switched_from_rt - Task switching away from RT class
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p) {
  /* Dequeue from RT if still queued */
  if (p->rt.on_rq) {
    dequeue_task_rt(rq, p, 0);
  }
}

/**
 * switched_to_rt - Task switching to RT class
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p) {
  /* Initialize RT entity */
  p->rt.time_slice = RR_TIMESLICE / NSEC_PER_MSEC;

  /* Check if we should preempt current */
  if (rq->curr != p) {
    check_preempt_curr_rt(rq, p, 0);
  }
}

/**
 * prio_changed_rt - RT task priority changed
 */
static void prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio) {
  struct sched_rt_entity *rt_se = &p->rt;

  if (!rt_se->on_rq)
    return;

  /* Re-enqueue at new priority */
  dequeue_task_rt(rq, p, 0);
  enqueue_task_rt(rq, p, 0);

  /* May need to preempt if priority increased */
  if (prio_less(p->prio, oldprio) && rq->curr != p) {
    check_preempt_curr_rt(rq, p, 0);
  }
}

/**
 * get_rr_interval_rt - Get round-robin interval for RT task
 */
static uint64_t get_rr_interval_rt(struct rq *rq, struct task_struct *p) {
  if (p->policy == SCHED_RR) {
    return RR_TIMESLICE;
  }
  return 0; /* SCHED_FIFO has no interval */
}

/**
 * update_curr_rt - Update current RT task's runtime
 */
static void update_curr_rt(struct rq *rq) {
  /* RT tasks don't track vruntime, but we could track bandwidth here */
}

/**
 * select_task_rq_rt - Select CPU for waking RT task
 */
static int select_task_rq_rt(struct task_struct *p, int cpu, int wake_flags) {
  /* Prefer the task's last CPU if allowed */
  if (cpumask_test_cpu(p->cpu, &p->cpus_allowed)) {
    return p->cpu;
  }

  /* Find first allowed CPU */
  return cpumask_first(&p->cpus_allowed);
}

/**
 * The RT scheduler class definition
 */
const struct sched_class rt_sched_class = {
    .next = &fair_sched_class,

    .enqueue_task = enqueue_task_rt,
    .dequeue_task = dequeue_task_rt,
    .yield_task = yield_task_rt,
    .check_preempt_curr = check_preempt_curr_rt,

    .pick_next_task = pick_next_task_rt,
    .put_prev_task = put_prev_task_rt,
    .set_next_task = set_next_task_rt,

    .task_tick = task_tick_rt,
    .task_fork = task_fork_rt,
    .task_dead = task_dead_rt,

    .switched_from = switched_from_rt,
    .switched_to = switched_to_rt,
    .prio_changed = prio_changed_rt,

    .get_rr_interval = get_rr_interval_rt,
    .update_curr = update_curr_rt,

    .select_task_rq = select_task_rq_rt,
};

/**
 * rt_init - Initialize RT scheduler
 */
void rt_init(void) {
  printk(KERN_INFO RT_CLASS "Real-time scheduler initialized\n");
}
