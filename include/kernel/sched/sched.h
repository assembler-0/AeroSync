#pragma once

#include <arch/x86_64/percpu.h>
#include <kernel/sched/cpumask.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>

/* Forward declarations */
struct sched_class;
struct fpu_state;

/* Task States */
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_ZOMBIE 3
#define TASK_STOPPED 4
#define TASK_DEAD 5

/* Enqueue/Dequeue Flags */
#define ENQUEUE_WAKEUP 0x01
#define ENQUEUE_RESTORE 0x02
#define ENQUEUE_MOVE 0x04
#define ENQUEUE_MIGRATED 0x08

#define DEQUEUE_SLEEP 0x01
#define DEQUEUE_SAVE 0x02
#define DEQUEUE_MOVE 0x04
#define DEQUEUE_MIGRATING 0x08
#define DEQUEUE_SKIP_NORM 0x10 /* Legacy: skip vruntime normalization */

/* Scheduling Policies */
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6

/* Task Flags */
#define PF_KTHREAD 0x00200000   /* I am a kernel thread */
#define PF_EXITING 0x00000004   /* Getting shut down */
#define PF_IDLE 0x00000010      /* I am an idle thread */
#define PF_WQ_WORKER 0x00000020 /* I'm a workqueue worker */
#define PF_VCPU 0x00000040      /* I'm a virtual CPU */
#define PF_NO_SETAFFINITY 0x400 /* Cannot set CPU affinity */

/* Wake Flags */
#define WF_SYNC 0x01
#define WF_FORK 0x02
#define WF_MIGRATED 0x04

/* Priority Macros */
#define MAX_USER_RT_PRIO 100
#define MAX_RT_PRIO MAX_USER_RT_PRIO
#define MAX_PRIO (MAX_RT_PRIO + 40)
#define DEFAULT_PRIO (MAX_RT_PRIO + 20)

/* Nice values range from -20 to 19 */
#define MIN_NICE (-20)
#define MAX_NICE 19
#define NICE_DEFAULT 0
#define NICE_0_LOAD 1024 /* The load weight of a task with nice 0 */
#define NICE_TO_PRIO_OFFSET 20

/* Scheduling time constants (in nanoseconds) */
#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL

/* RT scheduler constants */
#define MAX_RT_PRIO_LEVELS 100
#define RR_TIMESLICE (100 * NSEC_PER_MSEC) /* 100ms for SCHED_RR */

/* IPI Vector for Scheduler Reschedule */
#define IRQ_SCHED_IPI_VECTOR 0xEF

/*
 * The `prio_to_weight` table is a mapping from nice values to load weights.
 * prio_to_weight[20] corresponds to nice 0 (NICE_0_LOAD).
 * prio_to_weight[0] corresponds to nice -20 (highest priority).
 * prio_to_weight[39] corresponds to nice 19 (lowest priority).
 */
extern const unsigned int prio_to_weight[40];

DECLARE_PER_CPU(int, cpu_apic_id);

/* Represents a task's load weight */
struct load_weight {
  unsigned long weight;
  unsigned long inv_weight; /* Inverse for faster division */
};

/**
 * struct sched_entity - CFS scheduling entity
 *
 * Embedded in task_struct for tasks scheduled by CFS.
 */
struct sched_entity {
  struct rb_node run_node;
  struct list_head group_node;
  unsigned int on_rq;

  uint64_t exec_start_ns; /* When this entity started executing */
  uint64_t sum_exec_runtime;
  uint64_t vruntime;
  uint64_t prev_sum_exec_runtime;

  struct load_weight load; /* For CPU bandwidth distribution */
};

/**
 * struct sched_rt_entity - RT scheduling entity
 *
 * Embedded in task_struct for SCHED_FIFO and SCHED_RR tasks.
 */
struct sched_rt_entity {
  struct list_head run_list;
  unsigned int on_rq;
  unsigned int time_slice; /* Remaining time slice for SCHED_RR */
};

/**
 * struct sched_dl_entity - Deadline scheduling entity
 *
 * For SCHED_DEADLINE tasks (future implementation).
 */
struct sched_dl_entity {
  struct rb_node rb_node;
  uint64_t deadline;
  uint64_t runtime;
  uint64_t period;
  unsigned int on_rq;
};

/**
 * struct thread_struct - CPU context for context switching
 */
struct thread_struct {
  uint64_t rsp;
  uint64_t rip;
  uint64_t rbp;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t cr3;
  uint64_t rflags;
  /* FPU state - allocated separately for alignment */
  struct fpu_state *fpu;
  bool fpu_used; /* Lazy FPU: only save if used */
};

/**
 * struct task_struct - Task (process/thread) descriptor
 *
 * Central structure representing a schedulable entity in the kernel.
 */
struct task_struct {
  /*
   * Core scheduling fields - keep at top for cache efficiency
   */
  volatile long state;
  void *stack;
  unsigned int flags;

  /*
   * Preemption control
   */
  int preempt_count; /* 0 = preemptible, >0 = nested disable count */

  /*
   * Priority fields
   */
  int prio;                 /* Dynamic priority (effective priority) */
  int static_prio;          /* Nice value mapped to priority */
  int normal_prio;          /* Priority without PI boosting */
  unsigned int rt_priority; /* RT priority (0-99, 0 = highest) */
  int nice;                 /* Nice value for CFS (-20 to 19) */
  unsigned int policy;      /* Scheduling policy (SCHED_NORMAL, etc.) */

  /*
   * Scheduling class and entities
   */
  const struct sched_class *sched_class;
  struct sched_entity se;    /* CFS entity */
  struct sched_rt_entity rt; /* RT entity */
  struct sched_dl_entity dl; /* Deadline entity (future) */

  /*
   * CPU affinity
   */
  struct cpumask cpus_allowed; /* CPUs this task can run on */
  int nr_cpus_allowed;         /* Number of CPUs in cpus_allowed */
  int cpu;                     /* Current/last CPU */
  int node_id;                 /* NUMA node ID of the task (usually based on CPU) */

  /*
   * Task relationships
   */
  struct list_head tasks;    /* All tasks list */
  struct list_head run_list; /* Runqueue list (legacy) */

  /*
   * Memory management
   */
  struct mm_struct *mm;
  struct mm_struct *active_mm;

  /* Per-thread VMA Cache */
  #define MM_VMA_CACHE_SIZE 4
  struct vm_area_struct *vmacache[MM_VMA_CACHE_SIZE];
  uint64_t vmacache_seqnum;

  /*
   * Context for context switching
   */
  struct thread_struct thread;

  /*
   * Process identification
   */
  pid_t pid;
  pid_t tgid; /* Thread group ID */

  /*
   * Family relationships
   */
  struct task_struct *parent;
  struct list_head children;
  struct list_head sibling;

  /*
   * Task name and debugging
   */
  char comm[16]; /* Command name */

  /*
   * Statistics
   */
  uint64_t nvcsw;         /* Voluntary context switches */
  uint64_t nivcsw;        /* Involuntary context switches */
  uint64_t start_time_ns; /* Task start time */
};

/*
 * Preemption Control Macros
 *
 * These implement nested preemption disable/enable with automatic
 * reschedule on final enable if needed.
 */

/**
 * preempt_count - Get current preemption count
 */
#define preempt_count() (current->preempt_count)

/**
 * preempt_disable - Disable preemption
 *
 * Increments preempt_count. Can be nested.
 */
#define preempt_disable()                                                      \
  do {                                                                         \
    current->preempt_count++;                                                  \
    barrier();                                                                 \
  } while (0)

/**
 * preempt_enable_no_resched - Enable preemption without rescheduling
 *
 * Decrements preempt_count but doesn't check for pending reschedule.
 */
#define preempt_enable_no_resched()                                            \
  do {                                                                         \
    barrier();                                                                 \
    current->preempt_count--;                                                  \
  } while (0)

/**
 * preempt_enable - Enable preemption and reschedule if needed
 *
 * Decrements preempt_count and checks for pending reschedule.
 */
#define preempt_enable()                                                       \
  do {                                                                         \
    barrier();                                                                 \
    if (--current->preempt_count == 0 && this_cpu_read(need_resched))          \
      schedule();                                                              \
  } while (0)

/**
 * in_atomic - Check if we're in atomic context
 *
 * Returns true if preemption is disabled.
 */
#define in_atomic() (preempt_count() != 0)

/**
 * preemptible - Check if current context is preemptible
 */
#define preemptible() (preempt_count() == 0)

/*
 * Runqueue statistics
 */
struct rq_stats {
  uint64_t nr_switches;     /* Total context switches */
  uint64_t nr_migrations;   /* Tasks migrated to this CPU */
  uint64_t nr_load_balance; /* Load balance invocations */
  uint64_t exec_clock;      /* Total execution time (ns) */
  uint64_t wait_clock;      /* Total wait time (ns) */
};

/**
 * struct rt_rq - Real-time runqueue
 */
struct rt_rq {
  struct list_head queue[MAX_RT_PRIO_LEVELS];
  uint64_t bitmap[2]; /* Bitmap for quick priority lookup */
  unsigned int rt_nr_running;
  uint64_t rt_time;
  uint64_t rt_runtime; /* Max runtime per period (ns) */
  uint64_t rt_throttled;
  spinlock_t lock;
};

/**
 * struct cfs_rq - CFS runqueue
 */
struct cfs_rq {
  struct rb_root tasks_timeline;
  struct rb_node *rb_leftmost;
  struct load_weight load;
  unsigned int nr_running;
  uint64_t min_vruntime;
  uint64_t exec_clock;
};

/**
  * struct rq - Per-CPU runqueue
 */
struct rq {
  spinlock_t lock;
  unsigned int nr_running; /* Total runnable tasks */
  struct load_weight load; /* Instantaneous load weight */
  unsigned long avg_load;  /* Exponential Moving Average of load */

  /* Per-class runqueues */
  struct cfs_rq cfs;
  struct rt_rq rt;

  /* Legacy fields for compatibility */
  struct rb_root tasks_timeline; /* Direct access for fair.c */
  struct rb_node *rb_leftmost;

  struct task_struct *curr; /* Currently running task */
  struct task_struct *idle; /* This CPU's idle task */

  uint64_t clock;        /* Runqueue clock (ticks) */
  uint64_t clock_task;   /* Clock for task timing */
  uint64_t min_vruntime; /* CFS min_vruntime */
  uint64_t last_tick_ns;

  /* Statistics */
  struct rq_stats stats;

  /* CPU identification */
  int cpu;
};

/* Lock two runqueues in a stable order to prevent deadlocks */
void double_rq_lock(struct rq *rq1, struct rq *rq2);
void double_rq_unlock(struct rq *rq1, struct rq *rq2);

/* Global scheduler functions */
void schedule(void);
void set_need_resched(void);
void sched_init(void);
void sched_init_task(struct task_struct *initial_task);
void sched_init_ap(void);
void scheduler_tick(void);
void check_preempt(void);
void schedule_tail(struct task_struct *prev);

void set_task_nice(struct task_struct *p, int nice);

/* Scheduling policy functions */
int sched_setscheduler(struct task_struct *p, int policy, int priority);
int sched_getscheduler(struct task_struct *p);

/* Task state management functions */
void task_sleep(void);
void task_wake_up(struct task_struct *task);
void task_wake_up_all(void);

/* Helper to get current task */
extern struct task_struct *get_current(void);
#define current get_current()

/* Context Switch - returns the task that was switched out */
extern struct task_struct *switch_to(struct task_struct *prev,
                                     struct task_struct *next);

extern int cpu_id(void);

/* Per-CPU need_resched flag */
DECLARE_PER_CPU(int, need_resched);

/* Internal scheduler functions used by other scheduler modules */
extern void activate_task(struct rq *rq, struct task_struct *p);
extern void deactivate_task(struct rq *rq, struct task_struct *p);

/* Helper to get current runqueue */
extern struct rq *this_rq(void);
DECLARE_PER_CPU(struct rq, runqueues);

/* RT scheduler functions */
extern void enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags);
extern void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags);
extern struct task_struct *pick_next_task_rt(struct rq *rq);
extern void put_prev_task_rt(struct rq *rq, struct task_struct *p);
extern void task_tick_rt(struct rq *rq, struct task_struct *p, int queued);

/* Load balancing */
extern void reschedule_cpu(int cpu);

/* Statistics */
void sched_show_stats(void);
void sched_debug_task(struct task_struct *p);

/* Include scheduler class definitions after all types are defined */
#include <kernel/sched/sched_class.h>
