#pragma once

#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>

/* Task States */
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_ZOMBIE 3
#define TASK_STOPPED 4

/* Scheduling Policies */
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6

/* Task Flags */
#define PF_KTHREAD 0x00200000 /* I am a kernel thread */
#define PF_EXITING 0x00000004 /* getting shut down */

/* Priority Macros */
#define MAX_USER_RT_PRIO 100
#define MAX_RT_PRIO MAX_USER_RT_PRIO
#define MAX_PRIO (MAX_RT_PRIO + 40)
#define DEFAULT_PRIO (MAX_RT_PRIO + 20)

struct mm_struct; /* Forward declaration for memory management struct */

/* Represents a task's load weight */
struct load_weight {
  unsigned long weight;
  unsigned long inv_weight; // Inverse for faster division (optional, can be done with floating point or large scale integers)
};

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


/* Nice values range from -20 to 19 */
#define MIN_NICE (-20)
#define MAX_NICE 19
#define NICE_DEFAULT 0
#define NICE_0_LOAD 1024 /* The load weight of a task with nice 0 */
#define NICE_TO_PRIO_OFFSET 20

// IPI Vector for Scheduler Reschedule
#define IRQ_SCHED_IPI_VECTOR 0xEF // Using 239, typically a free vector


/*
 * The `prio_to_weight` table is a mapping from nice values to load weights.
 * This table is usually generated dynamically or is a constant array.
 * For now, we'll define a simplified version for common nice values.
 * In Linux, this is a much larger and more precise table.
 */
extern const unsigned int prio_to_weight[40];

extern int per_cpu_apic_id[MAX_CPUS];

struct task_struct {
  volatile long state;
  void *stack;
  unsigned int flags;
  int prio;
  int static_prio;
  int normal_prio;
  unsigned int rt_priority;
  int nice; // Nice value for CFS

  struct list_head tasks;

  struct mm_struct *mm;

  /* Scheduler information */
  struct sched_entity se;
  struct list_head run_list;

  /* Context for context switching */
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
  } thread;

  pid_t pid;
  pid_t tgid; // Thread group ID

  struct task_struct *parent;
  struct list_head children;
  struct list_head sibling;

  char comm[16]; // Command name
  int cpu;       // The CPU this task is currently running on/assigned to
};

/* Global scheduler functions */
void schedule(void);
void sched_init(void);
void sched_init_task(struct task_struct *initial_task);
void scheduler_tick(void);
void check_preempt(void);
void sched_dump_memory_stats(void);

/* Helper to get current task */
extern struct task_struct *get_current(void);
#define current get_current()

/* Context Switch - returns the task that was switched out */
extern struct task_struct *switch_to(struct task_struct *prev, struct task_struct *next);

/* Global scheduler lock for SMP operations (e.g., task migration) */
extern spinlock_t __rq_lock;

extern int cpu_id(void); // Declared globally now

/* CPU Specific runqueue */
struct rq {
  spinlock_t lock;
  unsigned int nr_running;
  struct rb_root tasks_timeline;
  struct rb_node *rb_leftmost; /* Cache for leftmost node */
  struct task_struct *curr;
  struct task_struct *idle;
  uint64_t clock;
  uint64_t min_vruntime;
  uint64_t last_tick_ns; /* Last time scheduler_tick updated in nanoseconds */
};

/* Per-CPU Runqueue Access */
/* This will need to be hooked up to your per-cpu data system */
// DECLARE_PER_CPU(struct rq, int runqueues);
