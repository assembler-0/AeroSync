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

struct sched_entity {
  struct rb_node run_node;
  struct list_head group_node;
  unsigned int on_rq;

  uint64_t exec_start;
  uint64_t sum_exec_runtime;
  uint64_t vruntime;
  uint64_t prev_sum_exec_runtime;
};

struct task_struct {
  volatile long state;
  void *stack;
  unsigned int flags;
  int prio;
  int static_prio;
  int normal_prio;
  unsigned int rt_priority;

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

/* CPU Specific runqueue */
struct rq {
  spinlock_t lock;
  unsigned int nr_running;
  struct rb_root tasks_timeline;
  struct task_struct *curr;
  struct task_struct *idle;
  uint64_t clock;
};

/* Per-CPU Runqueue Access */
/* This will need to be hooked up to your per-cpu data system */
// DECLARE_PER_CPU(struct rq, int runqueues);
