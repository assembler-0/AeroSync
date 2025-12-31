/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sched/process.c
 * @brief Process and thread management
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 */

#include <arch/x64/fpu.h>
#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/percpu.h>
#include <arch/x64/smp.h>
#include <kernel/fkx/fkx.h>
#include <kernel/sched/cpumask.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <lib/id_alloc.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <vsprintf.h>

/*
 * Process/Thread Management
 */

DECLARE_PER_CPU(struct rq, runqueues);

extern void ret_from_kernel_thread(void);

struct ida pid_ida;

void pid_allocator_init(void) {
  ida_init(&pid_ida, 32768);
  ida_alloc(&pid_ida); // Allocate 0 for idle/init
}

static pid_t alloc_pid(void) { return ida_alloc(&pid_ida); }

static void release_pid(pid_t pid) { ida_free(&pid_ida, pid); }

// Defined in switch.asm
struct task_struct *__switch_to(struct thread_struct *prev,
                                struct thread_struct *next);

struct task_struct *switch_to(struct task_struct *prev,
                              struct task_struct *next) {
  if (prev == next)
    return prev;

  return __switch_to(&prev->thread, &next->thread);
}

// Entry point for new kernel threads
void __used kthread_entry_stub(int (*threadfn)(void *data), void *data) {
  cpu_sti();
  threadfn(data);
  sys_exit(0);
}

struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...) {
  struct task_struct *ts = alloc_task_struct();
  if (!ts)
    return NULL;

  memset(ts, 0, sizeof(*ts));

  /* Allocate PID */
  ts->pid = alloc_pid();
  if (ts->pid < 0) {
    free_task_struct(ts);
    return NULL;
  }

  void *stack = vmalloc(PAGE_SIZE * 2);
  if (!stack) {
    release_pid(ts->pid);
    free_task_struct(ts);
    return NULL;
  }

  ts->stack = stack;
  ts->flags = PF_KTHREAD;
  ts->state = TASK_RUNNING;
  ts->mm = NULL;
  ts->active_mm = &init_mm;
  ts->cpu = cpu_id(); /* Default to current CPU, adjusted by balance */

  ts->nice = NICE_DEFAULT;
  ts->preempt_count = 0;

  /* Set default scheduler class to Fair */
  ts->sched_class = &fair_sched_class;
  ts->static_prio = NICE_DEFAULT + NICE_TO_PRIO_OFFSET;
  ts->se.load.weight = prio_to_weight[ts->static_prio];
  ts->se.vruntime = 0;

  /* Initialize CPU affinity */
  cpumask_setall(&ts->cpus_allowed);
  ts->nr_cpus_allowed = smp_get_cpu_count();

  va_list ap;
  va_start(ap, namefmt);
  vsnprintf(ts->comm, sizeof(ts->comm), namefmt, ap);
  va_end(ap);

  INIT_LIST_HEAD(&ts->run_list);
  INIT_LIST_HEAD(&ts->tasks);
  INIT_LIST_HEAD(&ts->sibling);
  INIT_LIST_HEAD(&ts->children);

  /* Initialize FPU state */
  ts->thread.fpu = fpu_alloc();
  if (!ts->thread.fpu) {
    vfree(ts->stack);
    release_pid(ts->pid);
    free_task_struct(ts);
    return NULL;
  }
  ts->thread.fpu_used = false;

  /* Setup stack for return to kthread_entry_stub via ret_from_kernel_thread */
  uint64_t *sp = (uint64_t *)((uint8_t *)stack + 8192);

  *(--sp) = (uint64_t)ret_from_kernel_thread;
  *(--sp) = 0;                  // rbx
  *(--sp) = 0;                  // rbp
  *(--sp) = (uint64_t)threadfn; // r12 -> rdi
  *(--sp) = (uint64_t)data;     // r13 -> rsi
  *(--sp) = 0;                  // r14
  *(--sp) = 0;                  // r15

  ts->thread.rsp = (uint64_t)sp;
  ts->thread.rflags = 0x202; /* IF enabled */

  return ts;
}
EXPORT_SYMBOL(kthread_create);

void kthread_run(struct task_struct *k) {
  if (!k)
    return;

  /* Use modern wake_up_new_task */
  wake_up_new_task(k);
}
EXPORT_SYMBOL(kthread_run);

struct task_struct *alloc_task_struct(void) {
  return kmalloc(sizeof(struct task_struct));
}

void free_task_struct(struct task_struct *task) {
  if (task) {
    kfree(task);
  }
}

void free_task(struct task_struct *task) {
  if (!task)
    return;

  if (task->pid >= 0) {
    release_pid(task->pid);
  }

  if (task->thread.fpu) {
    fpu_free(task->thread.fpu);
    task->thread.fpu = NULL;
  }

  if (task->stack) {
    vfree(task->stack);
    task->stack = NULL;
  }

  free_task_struct(task);
}

void wake_up_new_task(struct task_struct *p) {
  struct rq *rq;

#ifdef CONFIG_SMP
  /* Use class specific select_task_rq */
  int cpu = p->cpu;
  if (p->sched_class->select_task_rq) {
    cpu = p->sched_class->select_task_rq(p, cpu, WF_FORK);
  }
  set_task_cpu(p, cpu);
#endif

  rq = per_cpu_ptr(runqueues, p->cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  p->state = TASK_RUNNING;
  activate_task(rq, p);

  if (p->sched_class->check_preempt_curr) {
    p->sched_class->check_preempt_curr(rq, p, WF_FORK);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

struct task_struct *copy_process(unsigned long clone_flags,
                                 struct task_struct *p) {
  struct task_struct *ts = alloc_task_struct();
  if (!ts)
    return NULL;

  memcpy(ts, p, sizeof(struct task_struct));

  ts->pid = alloc_pid();
  ts->stack = vmalloc(PAGE_SIZE * 2);
  if (!ts->stack) {
    release_pid(ts->pid);
    free_task_struct(ts);
    return NULL;
  }

  memcpy(ts->stack, p->stack, PAGE_SIZE * 2);

  uint64_t stack_offset = (uint64_t)p->thread.rsp - (uint64_t)p->stack;
  ts->thread.rsp = (uint64_t)ts->stack + stack_offset;

  if (clone_flags & CLONE_VM) {
    ts->mm = p->mm;
  } else if (p->mm) {
    ts->mm = mm_copy(p->mm);
  } else {
    ts->mm = NULL;
  }

  ts->active_mm = ts->mm ? ts->mm : p->active_mm;

  /* Copy FPU */
  ts->thread.fpu = fpu_alloc();
  if (!ts->thread.fpu) {
    /* cleanup */
    return NULL;
  }
  if (p->thread.fpu && p->thread.fpu_used) {
    fpu_copy(ts->thread.fpu, p->thread.fpu);
    ts->thread.fpu_used = true;
  } else {
    fpu_init_task(ts->thread.fpu);
    ts->thread.fpu_used = false;
  }

  INIT_LIST_HEAD(&ts->tasks);
  INIT_LIST_HEAD(&ts->children);
  INIT_LIST_HEAD(&ts->sibling);
  INIT_LIST_HEAD(&ts->run_list);

  return ts;
}

struct task_struct *process_spawn(int (*entry)(void *), void *data,
                                  const char *name) {
  struct task_struct *ts = alloc_task_struct();
  if (!ts)
    return NULL;

  memset(ts, 0, sizeof(*ts));

  ts->mm = mm_create();
  if (!ts->mm) {
    free_task_struct(ts);
    return NULL;
  }
  ts->active_mm = ts->mm;

  ts->stack = vmalloc(PAGE_SIZE * 2);
  if (!ts->stack) {
    mm_free(ts->mm);
    free_task_struct(ts);
    return NULL;
  }

  ts->pid = alloc_pid();

  /* Affinity init */
  cpumask_setall(&ts->cpus_allowed);

  /* Scheduler init */
  ts->sched_class = &fair_sched_class;
  ts->cpu = 0; /* Let balancer fix it */
  ts->nice = 0;
  ts->static_prio = NICE_TO_PRIO_OFFSET;
  ts->se.load.weight = prio_to_weight[ts->static_prio];

  strncpy(ts->comm, name, sizeof(ts->comm));

  /* FPU */
  ts->thread.fpu = fpu_alloc();
  ts->thread.fpu_used = false;

  uint64_t *sp = (uint64_t *)((uint8_t *)ts->stack + 8192);
  *(--sp) = (uint64_t)ret_from_kernel_thread;
  *(--sp) = 0;               // rbx
  *(--sp) = 0;               // rbp
  *(--sp) = (uint64_t)entry; // r12
  *(--sp) = (uint64_t)data;  // r13
  *(--sp) = 0;               // r14
  *(--sp) = 0;               // r15

  ts->thread.rsp = (uint64_t)sp;

  wake_up_new_task(ts);
  return ts;
}
EXPORT_SYMBOL(process_spawn);

pid_t sys_fork(void) {
  struct task_struct *curr = get_current();
  struct task_struct *child = copy_process(0, curr);
  if (!child)
    return -1;

  pid_t pid = child->pid;
  wake_up_new_task(child);

  return pid;
}

void sys_exit(int error_code) {
  struct task_struct *curr = get_current();
  curr->state = TASK_ZOMBIE;
  schedule();
  while (1)
    ;
}
