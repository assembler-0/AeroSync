#include <kernel/classes.h>
#include <kernel/sched/sched.h>
#include <lib/printk.h>
#include <../../include/linux/rbtree.h>

/*
 * Completely Fair Scheduler (Simplified)
 */

/*
 * Enqueue a task into the rb-tree
 */
static void __enqueue_entity(struct rq *rq, struct sched_entity *se) {
  struct rb_node **link = &rq->tasks_timeline.rb_node;
  struct rb_node *parent = NULL;
  struct sched_entity *entry;
  int leftmost = 1;

  while (*link) {
    parent = *link;
    entry = rb_entry(parent, struct sched_entity, run_node);

    /*
     * We traverse the tree. Tasks with smaller vruntime go left.
     */
    if (se->vruntime < entry->vruntime) {
      link = &parent->rb_left;
    } else {
      link = &parent->rb_right;
      leftmost = 0;
    }
  }

  rb_link_node(&se->run_node, parent, link);
  rb_insert_color(&se->run_node, &rq->tasks_timeline);
}

static void __dequeue_entity(struct rq *rq, struct sched_entity *se) {
  rb_erase(&se->run_node, &rq->tasks_timeline);
}

/*
 * Update vruntime
 */
static void update_curr(struct rq *rq) {
  struct task_struct *curr = rq->curr;
  uint64_t delta_exec;

  if (!curr)
    return;

  // In a real kernel, we'd use high-res nanoseconds.
  // Here we might just use ticks if that's all we have,
  // but ideally we read TSC.
  // For simplicity, let's assume 1 tick = 1 "unit" of vruntime for now
  // or use a TSC delta.

  delta_exec = 1; // Simplified: 1 tick

  curr->se.sum_exec_runtime += delta_exec;
  curr->se.vruntime += delta_exec;
}

void task_tick_fair(struct rq *rq, struct task_struct *curr) {
  // If the task is in the tree, remove before modifying vruntime
  if (curr->se.on_rq) {
    __dequeue_entity(rq, &curr->se);
  }

  update_curr(rq);

  if (curr->se.on_rq) {
    __enqueue_entity(rq, &curr->se);
  }

  // Potential preemption decision can be added here later
}

struct task_struct *pick_next_task_fair(struct rq *rq) {
  struct rb_node *leftmost = rb_first(&rq->tasks_timeline);

  if (!leftmost)
    return NULL;

  struct sched_entity *se = rb_entry(leftmost, struct sched_entity, run_node);
  return container_of(se, struct task_struct, se);
}

/*
 * Public API to wake up a task (add directly to rq)
 */
void activate_task(struct rq *rq, struct task_struct *p) {
  if (p->se.on_rq)
    return;

  p->se.on_rq = 1;
  rq->nr_running++;
  __enqueue_entity(rq, &p->se);
}

void deactivate_task(struct rq *rq, struct task_struct *p) {
  if (!p->se.on_rq)
    return;

  p->se.on_rq = 0;
  rq->nr_running--;
  __dequeue_entity(rq, &p->se);
}
