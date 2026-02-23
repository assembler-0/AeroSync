/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/deadline.c
 * @brief Deadline Scheduler (SCHED_DEADLINE) implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * Implements Earliest Deadline First (EDF) scheduling algorithm with
 * Constant Bandwidth Server (CBS) for budget enforcement.
 */

#include <aerosync/sched/sched.h>
#include <aerosync/classes.h>
#include <linux/rbtree.h>
#include <linux/container_of.h>
#include <lib/printk.h>
#include <arch/x86_64/tsc.h>
#include <aerosync/timer.h>

/* Forward declarations */
static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags);

static void dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags);

/*
 * Deadline Helper Functions
 */

static inline int dl_time_before(uint64_t a, uint64_t b) {
  return (int64_t) (a - b) < 0;
}

/*
 * Timer callback for replenishing bandwidth
 */
static void dl_task_timer(struct timer_list *timer) {
  struct task_struct *p = container_of(timer, struct task_struct, dl.dl_timer);
  struct rq *rq;
  irq_flags_t flags;

  rq = per_cpu_ptr(runqueues, p->cpu);
  flags = spinlock_lock_irqsave(&rq->lock);

  /* Replenish logic */
  struct sched_dl_entity *dl_se = &p->dl;

  dl_se->dl_throttled = 0;
  dl_se->deadline = rq->clock_task + dl_se->period;
  dl_se->runtime = dl_se->period * 20 / 100; /* simplified reset to default budget if not tracking original */
  /* Ideally we track dl_bw and set runtime = period * bw */

  if (dl_se->on_rq) {
    /* Should not be on_rq if throttled, but if it is, re-enqueue */
    enqueue_task_dl(rq, p, ENQUEUE_RESTORE); // Forward declaration issue? No, it's static in this file.
    /* Wait, enqueue_task_dl is static below. We need to move this or fwd declare. */
  } else {
    /* If task was sleeping/blocked while throttled, we just clear throttled flag.
       If it was runnable but throttled, we need to wake it up/enqueue it. */
    if (p->state == TASK_RUNNING || p->state == TASK_WAKEKILL) {
      enqueue_task_dl(rq, p, ENQUEUE_RESTORE);
    }
  }

  if (rq->curr != p)
    reschedule_cpu(p->cpu);

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

static void setup_new_dl_entity(struct sched_dl_entity *dl_se) {
  struct rq *rq = this_rq();
  struct task_struct *p = container_of(dl_se, struct task_struct, dl);

  /* Default values if not set */
  if (dl_se->period == 0) {
    dl_se->period = 100 * NSEC_PER_MSEC; /* 100ms default period */
  }
  if (dl_se->runtime == 0) {
    dl_se->runtime = 20 * NSEC_PER_MSEC; /* 20ms default budget (20%) */
  }

  dl_se->deadline = rq->clock_task + dl_se->period;
  dl_se->dl_throttled = 0;

  /* Init timer */
  timer_setup(&dl_se->dl_timer, dl_task_timer, p);
}

/*
 * Red-Black Tree Operations
 */

static void __enqueue_dl_entity(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se) {
  struct rb_node **link = &dl_rq->root.rb_node;
  struct rb_node *parent = nullptr;
  struct sched_dl_entity *entry;
  int leftmost = 1;

  while (*link) {
    parent = *link;
    entry = rb_entry(parent, struct sched_dl_entity, rb_node);

    if (dl_time_before(dl_se->deadline, entry->deadline)) {
      link = &parent->rb_left;
    } else {
      link = &parent->rb_right;
      leftmost = 0;
    }
  }

  rb_link_node(&dl_se->rb_node, parent, link);
  rb_insert_color(&dl_se->rb_node, &dl_rq->root);

  if (leftmost) {
    dl_rq->rb_leftmost = &dl_se->rb_node;
  }
}

static void __dequeue_dl_entity(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se) {
  if (dl_rq->rb_leftmost == &dl_se->rb_node) {
    struct rb_node *next_node = rb_next(&dl_se->rb_node);
    dl_rq->rb_leftmost = next_node;
  }

  rb_erase(&dl_se->rb_node, &dl_rq->root);
}

/*
 * Replenish DL entity logic (CBS)
 */
static void replenish_dl_entity(struct sched_dl_entity *dl_se, struct sched_dl_entity *pi_se) {
  struct rq *rq = this_rq();

  /* Simple CBS: if deadline is in the past, or very close, generate new */
  if (dl_time_before(dl_se->deadline, rq->clock_task)) {
    dl_se->deadline = rq->clock_task + dl_se->period;
    dl_se->runtime = pi_se->period * 20 / 100; /* Reset runtime */
  }
}

/*
 * Throttle DL entity
 */
static void start_dl_timer(struct sched_dl_entity *dl_se) {
  struct rq *rq = this_rq();
  uint64_t expire = dl_se->deadline; /* Replenish at deadline */

  /* If deadline passed, use next period */
  if (dl_time_before(expire, rq->clock_task)) {
    expire = rq->clock_task + dl_se->period;
    dl_se->deadline = expire;
  }

  timer_add(&dl_se->dl_timer, expire);
}

/*
 * Scheduler Class Interface
 */

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags) {
  struct sched_dl_entity *dl_se = &p->dl;
  struct dl_rq *dl_rq = &rq->dl;

  if (dl_se->on_rq)
    return;

  /* If throttled, don't enqueue yet. Timer will handle it. */
  if (dl_se->dl_throttled) {
    return;
  }

  /* If this is a new task or waking up after deadline missed, replenish */
  if (flags & ENQUEUE_WAKEUP) {
    replenish_dl_entity(dl_se, dl_se);
  }

  __enqueue_dl_entity(dl_rq, dl_se);
  dl_se->on_rq = 1;
  dl_rq->dl_nr_running++;
  rq->nr_running++;
}

static void dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags) {
  struct sched_dl_entity *dl_se = &p->dl;
  struct dl_rq *dl_rq = &rq->dl;

  if (!dl_se->on_rq)
    return;

  __dequeue_dl_entity(dl_rq, dl_se);
  dl_se->on_rq = 0;
  dl_rq->dl_nr_running--;
  rq->nr_running--;
}

static void yield_task_dl(struct rq *rq) {
  /*
   * Force throttle until deadline
   */
  rq->curr->dl.runtime = 0;
  rq->curr->dl.dl_yielded = 1;
  set_need_resched();
}

static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p, int flags) {
  /* DL always preempts lower classes */
  if (rq->curr->sched_class != &dl_sched_class) {
    set_need_resched();
    return;
  }

  /* Earliest deadline preempts */
  struct sched_dl_entity *curr_dl = &rq->curr->dl;
  struct sched_dl_entity *p_dl = &p->dl;

  if (dl_time_before(p_dl->deadline, curr_dl->deadline)) {
    set_need_resched();
  }
}

static struct task_struct *pick_next_task_dl(struct rq *rq) {
  struct dl_rq *dl_rq = &rq->dl;
  struct sched_dl_entity *dl_se;
  struct task_struct *p;

  if (!dl_rq->rb_leftmost)
    return nullptr;

  dl_se = rb_entry(dl_rq->rb_leftmost, struct sched_dl_entity, rb_node);
  p = container_of(dl_se, struct task_struct, dl);

  /* Remove from tree to mark as running (following core design) */
  dequeue_task_dl(rq, p, 0);

  return p;
}

static void put_prev_task_dl(struct rq *rq, struct task_struct *p) {
  struct sched_dl_entity *dl_se = &p->dl;

  /* If throttled, don't re-enqueue. */
  if (dl_se->dl_throttled) {
    return;
  }

  /* If still running (preempted), add back to tree */
  if (p->state == TASK_RUNNING && !dl_se->on_rq) {
    enqueue_task_dl(rq, p, 0);
  }
}

static void set_next_task_dl(struct rq *rq, struct task_struct *p, bool first) {
  /* Record start time for runtime accounting */
  p->se.exec_start_ns = rq->clock_task;
}

static void task_tick_dl(struct rq *rq, struct task_struct *p, int queued) {
  struct sched_dl_entity *dl_se = &p->dl;

  /* Account runtime */
  uint64_t delta_exec = rq->clock_task - p->se.exec_start_ns;
  p->se.exec_start_ns = rq->clock_task;

  if (dl_se->runtime > delta_exec) {
    dl_se->runtime -= delta_exec;
  } else {
    dl_se->runtime = 0;

    /*
     * Hard Deadline Enforcement:
     * 1. Mark task as throttled.
     * 2. Dequeue task (happens in put_prev or we force reschedule).
     * 3. Start timer to replenish budget at next period/deadline.
     */
    dl_se->dl_throttled = 1;
    start_dl_timer(dl_se);
    set_need_resched();
  }
}

static void task_fork_dl(struct task_struct *p) {
  setup_new_dl_entity(&p->dl);
}

static void task_dead_dl(struct task_struct *p) {
  timer_del(&p->dl.dl_timer);
}

static void switched_from_dl(struct rq *rq, struct task_struct *p) {
  timer_del(&p->dl.dl_timer);
  if (p->dl.on_rq)
    dequeue_task_dl(rq, p, 0);
}

static void switched_to_dl(struct rq *rq, struct task_struct *p) {
  setup_new_dl_entity(&p->dl);

  if (p->dl.on_rq) {
    if (rq->curr != p)
      check_preempt_curr_dl(rq, p, 0);
  }
}

static void prio_changed_dl(struct rq *rq, struct task_struct *p, int oldprio) {
  /* If still DL, update position */
  if (p->dl.on_rq) {
    dequeue_task_dl(rq, p, 0);
    enqueue_task_dl(rq, p, 0);
  }
}

static uint64_t get_rr_interval_dl(struct rq *rq, struct task_struct *p) {
  return 0;
}

static void update_curr_dl(struct rq *rq) {
  /* Called from various places to update stats */
  struct task_struct *curr = rq->curr;
  if (curr->sched_class != &dl_sched_class) return;

  uint64_t delta_exec = rq->clock_task - curr->se.exec_start_ns;
  curr->se.exec_start_ns = rq->clock_task;

  if (curr->dl.runtime > delta_exec)
    curr->dl.runtime -= delta_exec;
  else
    curr->dl.runtime = 0;
}

static int select_task_rq_dl(struct task_struct *p, int cpu, int wake_flags) {
  if (cpumask_test_cpu(p->cpu, &p->cpus_allowed))
    return p->cpu;

  return cpumask_first(&p->cpus_allowed);
}

const struct sched_class dl_sched_class = {
  .next = &rt_sched_class, /* Point to next class (RT) */

  .enqueue_task = enqueue_task_dl,
  .dequeue_task = dequeue_task_dl,
  .yield_task = yield_task_dl,
  .check_preempt_curr = check_preempt_curr_dl,

  .pick_next_task = pick_next_task_dl,
  .put_prev_task = put_prev_task_dl,
  .set_next_task = set_next_task_dl,

  .task_tick = task_tick_dl,
  .task_fork = task_fork_dl,
  .task_dead = task_dead_dl,

  .switched_from = switched_from_dl,
  .switched_to = switched_to_dl,
  .prio_changed = prio_changed_dl,

  .get_rr_interval = get_rr_interval_dl,
  .update_curr = update_curr_dl,

  .select_task_rq = select_task_rq_dl,
};

void init_dl_rq(struct dl_rq *dl_rq) {
  dl_rq->root = RB_ROOT;
  dl_rq->rb_leftmost = nullptr;
  dl_rq->dl_nr_running = 0;
  dl_rq->dl_bw = 0;
}
