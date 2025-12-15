#include <linux/container_of.h>
#include <kernel/sched/sched.h>
#include <linux/rbtree.h>
#include <arch/x64/tsc.h> // For get_time_ns
#include <lib/math.h>

/*
 * This table maps nice values (-20 to 19) to their corresponding load weights.
 * prio_to_weight[20] corresponds to nice 0 (NICE_0_LOAD).
 * prio_to_weight[0] corresponds to nice -20 (highest priority).
 * prio_to_weight[39] corresponds to nice 19 (lowest priority).
 */
const uint32_t prio_to_weight[40] = {
    /* -20 */     88761,     71755,     56483,     46273,     36291,
    /* -15 */     29154,     23254,     18705,     14949,     11916,
    /* -10 */      9548,      7620,      6100,      4904,      3906,
    /*  -5 */      3121,      2501,      1991,      1586,      1277,
    /*   0 */      1024,       820,       655,       526,       423,
    /*   5 */       335,       272,       215,       172,       137,
    /*  10 */       110,        87,        70,        56,        45,
    /*  15 */        36,        29,        23,        18,        15,
};


/*
 * Completely Fair Scheduler (CFS)
 *
 * This implementation tracks vruntime (virtual runtime) of tasks.
 * Tasks are ordered by vruntime in a Red-Black Tree.
 * The task with the smallest vruntime is the "leftmost" node and is picked next.
 */

/*
 * Update the min_vruntime of the runqueue.
 * min_vruntime tracks the monotonic progress of the system's virtual time.
 * It is calculated as the minimum of:
 * 1. The currently running task's vruntime (if any)
 * 2. The leftmost (soonest to run) task's vruntime in the tree
 */
static void update_min_vruntime(struct rq *rq) {
  uint64_t vruntime = rq->min_vruntime;

  /*
   * If there is a current task, start with its vruntime.
   */
  if (rq->curr)
    vruntime = rq->curr->se.vruntime;

  /*
   * If there are tasks in the tree, compare with the leftmost one.
   */
  if (rq->rb_leftmost) {
    struct sched_entity *se = rb_entry(rq->rb_leftmost, struct sched_entity, run_node);
    if (!rq->curr)
      vruntime = se->vruntime;
    else
      if (se->vruntime < vruntime)
        vruntime = se->vruntime;
  }

  /*
   * Ensure min_vruntime only moves forward.
   */
  if (vruntime > rq->min_vruntime)
    rq->min_vruntime = vruntime;
}

/*
 * Calculate delta_vruntime based on actual execution time and load weight.
 * vruntime += delta_exec_ns * NICE_0_LOAD / current_load_weight
 * This ensures that tasks with higher weight (lower nice) have their vruntime
 * advance slower, thus getting more CPU time.
 */
static uint64_t __calc_delta(uint64_t delta_exec_ns, unsigned long weight) {
  if (weight == 0)
    return delta_exec_ns;
  unsigned __int128 prod = (unsigned __int128)delta_exec_ns * (unsigned __int128)NICE_0_LOAD;
  return (uint64_t)(prod / (unsigned __int128)weight);
}


/*
 * Enqueue a task into the rb-tree and update rb_leftmost cache
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

  if (leftmost)
    rq->rb_leftmost = &se->run_node;
}

static void __dequeue_entity(struct rq *rq, struct sched_entity *se) {
  if (rq->rb_leftmost == &se->run_node) {
    struct rb_node *next = rb_next(&se->run_node);
    rq->rb_leftmost = next;
  }

  rb_erase(&se->run_node, &rq->tasks_timeline);
}

/*
 * Update vruntime of the current task.
 * This function calculates the delta internally based on exec_start_ns.
 */
static void update_curr(struct rq *rq) {
  struct task_struct *curr = rq->curr;
  uint64_t now_ns = get_time_ns();
  uint64_t delta_exec_ns;
  uint64_t delta_vruntime;

  if (!curr)
    return;
  
  if (now_ns < curr->se.exec_start_ns) { // TSC wrap around or unexpected time change
      delta_exec_ns = 0; 
  } else {
      delta_exec_ns = now_ns - curr->se.exec_start_ns;
  }

  curr->se.sum_exec_runtime += delta_exec_ns;
  
  // Scale delta_exec_ns by load weight to get delta_vruntime
  delta_vruntime = __calc_delta(delta_exec_ns, curr->se.load.weight);
  curr->se.vruntime += delta_vruntime;
  
  // Reset exec_start_ns for the next period
  curr->se.exec_start_ns = now_ns;

  /* Update the runqueue's min_vruntime to track progress */
  update_min_vruntime(rq);
}

/*
 * Place a task into the timeline.
 * Used for new tasks or waking tasks to ensure they don't starve others
 * or get starved.
 */
static void place_entity(struct rq *rq, struct sched_entity *se, int initial) {
  uint64_t vruntime = rq->min_vruntime;

  /*
   * For a new task (initial=1), we want to place it slightly behind min_vruntime
   * so it doesn't immediately preempt everyone if the system is loaded,
   * but usually in CFS we place it at min_vruntime.
   * 
   * For waking tasks, we penalize them slightly to prevent sleep/wake loops
   * from monopolizing CPU (though strict CFS just uses min_vruntime).
   */
   
  if (initial) {
    vruntime += 1; // Small penalty/offset for new tasks
  } else {
    /* 
     * Waking task. Ensure vruntime is at least min_vruntime.
     * Use existing vruntime if it's larger (task consumed its slice previously).
     */
    if (se->vruntime < vruntime)
      se->vruntime = vruntime;
  }

  se->vruntime = max(se->vruntime, vruntime);
}

/*
 * Called periodically by the timer interrupt
 */
void task_tick_fair(struct rq *rq, struct task_struct *curr) {
  update_curr(rq);

  /*
   * If the current task is running, we might need to re-insert it
   * into the tree to maintain order if its vruntime has grown larger
   * than others.
   */
  if (curr->se.on_rq) {
    __dequeue_entity(rq, &curr->se);
    __enqueue_entity(rq, &curr->se);
  }
}

struct task_struct *pick_next_task_fair(struct rq *rq) {
  struct rb_node *leftmost = rq->rb_leftmost;

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
    
  /*
   * Update stats and place the entity fairly
   */
  if (p->state == TASK_RUNNING) { 
     /* 
      * Technically activate_task is called for waking up or new tasks.
      * If it's a new task (vruntime=0 usually), we treat it as initial.
      */
     int initial = (p->se.vruntime == 0);
     place_entity(rq, &p->se, initial);
  }
  p->se.exec_start_ns = get_time_ns(); // Set exec_start_ns when activated

  p->se.on_rq = 1;
  rq->nr_running++;
  __enqueue_entity(rq, &p->se);
}

void deactivate_task(struct rq *rq, struct task_struct *p) {
  if (!p->se.on_rq)
    return;

  /*
   * Update runtime one last time before sleeping
   */
  if (rq->curr == p) {
    update_curr(rq);
  }

  p->se.on_rq = 0;
  rq->nr_running--;
  __dequeue_entity(rq, &p->se);
}