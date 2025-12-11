#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <lib/printk.h>
#include <mm/slab.h>

/*
 * Scheduler Core Implementation
 */

// Simple fixed-size runqueue array for now (per CPU)
// In a real implementation this should be dynamically allocated per CPU
#define MAX_CPUS 32
static struct rq per_cpu_runqueues[MAX_CPUS];

// Current task per CPU (lookup table)
static struct task_struct *current_tasks[MAX_CPUS];

// Preemption flag per CPU
static volatile int need_resched[MAX_CPUS];

/*
 * Basic Helpers
 */

static inline int cpu_id(void) {
  // TODO: Map LAPIC ID to logical ID if sparse
  return (int)smp_get_id();
}

struct rq *this_rq(void) { return &per_cpu_runqueues[cpu_id()]; }

struct task_struct *get_current(void) { return current_tasks[cpu_id()]; }

void set_current(struct task_struct *t) { current_tasks[cpu_id()] = t; }

/*
 * Scheduler Initialization
 */
void sched_init(void) {
  int i;
  for (i = 0; i < MAX_CPUS; i++) {
    struct rq *rq = &per_cpu_runqueues[i];
    rq->lock = 0; // Init spinlock
    rq->nr_running = 0;
    rq->tasks_timeline = RB_ROOT;
    rq->clock = 0;
    // Idle task will be set later (usually the boot thread becomes idle)
  }

  printk(SCHED_CLASS "Scheduler initialized for %d logical CPUs slots.\n",
         MAX_CPUS);
  printk(SCHED_CLASS "Using slab allocator for task management\n");
}

/*
 * External function to pick the next task (CFS)
 * Implemented in fair.c
 */
extern struct task_struct *pick_next_task_fair(struct rq *rq);
extern void task_tick_fair(struct rq *rq, struct task_struct *curr);

/*
 * Context Switch
 */
extern void switch_to(struct task_struct *prev, struct task_struct *next);

/*
 * The main schedule function
 */
void schedule(void) {
  struct task_struct *prev, *next;
  struct rq *rq = this_rq();

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);
  prev = rq->curr;

  // Pick next task
  next = pick_next_task_fair(rq);

  if (!next) {
    next = rq->idle;
  }

  if (prev != next) {
    rq->curr = next;
    set_current(next);
    // Release the runqueue lock before switching, but keep IRQs disabled
    // until after the context switch completes to avoid handling interrupts
    // in the middle of a switch.
    spinlock_unlock(&rq->lock);
    switch_to(prev, next);
    restore_irq_flags(flags);
    return;
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Called from APIC timer interrupt
 */
void scheduler_tick(void) {
  struct rq *rq = this_rq();
  struct task_struct *curr = rq->curr;

  spinlock_lock((volatile int *)&rq->lock);

  rq->clock++;

  if (curr) {
    task_tick_fair(rq, curr);
  }

  // Set preemption flag
  need_resched[cpu_id()] = 1;

  spinlock_unlock((volatile int *)&rq->lock);
}

/*
 * Check if preemption is needed and schedule if safe
 */
void check_preempt(void) {
  if (need_resched[cpu_id()]) {
    need_resched[cpu_id()] = 0;
    schedule();
  }
}

/*
 * Initialize the first task (idle/init) for BSP
 */
void sched_init_task(struct task_struct *initial_task) {
  struct rq *rq = this_rq();
  
  // Initialize the task's scheduler entity
  initial_task->se.vruntime = 0;
  initial_task->se.on_rq = 0;
  initial_task->state = TASK_RUNNING;
  
  rq->curr = initial_task;
  rq->idle = initial_task;
  set_current(initial_task);
  
  printk(SCHED_CLASS "Initial task initialized with slab-managed memory\n");
}

/*
 * Scheduler memory statistics
 */
void sched_dump_memory_stats(void) {
  slab_dump_stats();
}
