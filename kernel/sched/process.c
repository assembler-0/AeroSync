/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sched/process.c
 * @brief Process and thread management (Linux-like backend)
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/x64/cpu.h>
#include <arch/x64/entry.h>
#include <arch/x64/fpu.h>
#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
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
#include <lib/vsprintf.h>

/*
 * Process/Thread Management
 */

DECLARE_PER_CPU(struct rq, runqueues);

extern void ret_from_kernel_thread(void);
extern void ret_from_user_thread(void);

struct ida pid_ida;

/* Global list of all tasks in the system */
LIST_HEAD(task_list);
static spinlock_t tasklist_lock = 0;

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
  threadfn(data);
  sys_exit(0);
}

/*
 * copy_process - The core of fork/clone/kthread_create.
 * Creates a new task and copies/shares resources from the parent.
 */
struct task_struct *copy_process(uint64_t clone_flags,
                                uint64_t stack_start,
                                struct task_struct *parent) {
  struct task_struct *p;

  p = alloc_task_struct();
  if (!p)
      return NULL;

  memset(p, 0, sizeof(*p));

  p->pid = alloc_pid();
  if (p->pid < 0) {
      free_task_struct(p);
      return NULL;
  }

  // Allocate 16KB kernel stack
  p->stack = vmalloc(PAGE_SIZE * 4);
  if (!p->stack) {
      release_pid(p->pid);
      free_task_struct(p);
      return NULL;
  }

  // Initialize basic fields
  p->state = TASK_RUNNING;
  p->cpu = cpu_id();
  p->flags = 0;
  p->preempt_count = 0;
  p->parent = parent;
  
  INIT_LIST_HEAD(&p->tasks);
  INIT_LIST_HEAD(&p->children);
  INIT_LIST_HEAD(&p->sibling);
  INIT_LIST_HEAD(&p->run_list);

  // Setup memory management
  if (clone_flags & CLONE_VM) {
      p->mm = parent->mm;
  } else if (parent->mm) {
      p->mm = mm_copy(parent->mm);
      if (!p->mm) {
          vfree(p->stack);
          release_pid(p->pid);
          free_task_struct(p);
          return NULL;
      }
  }
  p->active_mm = p->mm ? p->mm : parent->active_mm;

  // Setup FPU
  p->thread.fpu = fpu_alloc();
  if (parent->thread.fpu && parent->thread.fpu_used) {
      fpu_copy(p->thread.fpu, parent->thread.fpu);
      p->thread.fpu_used = true;
  } else {
      fpu_init_task(p->thread.fpu);
      p->thread.fpu_used = false;
  }

  // Setup scheduler class and priority
  p->sched_class = parent->sched_class;
  p->static_prio = parent->static_prio;
  p->nice = parent->nice;
  p->se.load = parent->se.load;
  cpumask_copy(&p->cpus_allowed, &parent->cpus_allowed);

  // Link into global lists
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_add_tail(&p->tasks, &task_list);
  if (parent) {
      list_add_tail(&p->sibling, &parent->children);
  }
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  return p;
}

struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...) {
  struct task_struct *curr = get_current();
  struct task_struct *p = copy_process(CLONE_VM, 0, curr);
  if (!p) return NULL;

  p->flags |= PF_KTHREAD;
  p->mm = NULL; // Kernel threads have no user MM
  
  va_list ap;
  va_start(ap, namefmt);
  vsnprintf(p->comm, sizeof(p->comm), namefmt, ap);
  va_end(ap);

  // Setup stack for ret_from_kernel_thread
  uint64_t *sp = (uint64_t *)((uint8_t *)p->stack + (PAGE_SIZE * 4));
  *(--sp) = (uint64_t)ret_from_kernel_thread;
  *(--sp) = 0;                  // rbx
  *(--sp) = 0;                  // rbp
  *(--sp) = (uint64_t)threadfn; // r12
  *(--sp) = (uint64_t)data;     // r13
  *(--sp) = 0;                  // r14
  *(--sp) = 0;                  // r15

  p->thread.rsp = (uint64_t)sp;
  p->thread.rflags = 0x202;

  return p;
}
EXPORT_SYMBOL(kthread_create);

void kthread_run(struct task_struct *k) {
  if (k) wake_up_new_task(k);
}
EXPORT_SYMBOL(kthread_run);

pid_t do_fork(uint64_t clone_flags, uint64_t stack_start) {
    struct task_struct *curr = get_current();
    struct task_struct *p = copy_process(clone_flags, stack_start, curr);
    if (!p) return -1;

    pid_t pid = p->pid;
    
    // For fork, we need to clone the stack more carefully
    memcpy(p->stack, curr->stack, PAGE_SIZE * 4);
    
    uint64_t stack_offset = (uint64_t)curr->thread.rsp - (uint64_t)curr->stack;
    p->thread.rsp = (uint64_t)p->stack + stack_offset;
    
    // Child returns 0 in RAX. 
    // Since we don't have a pt_regs offset yet, we assume the switch_to context
    // is at the same place. In a real kernel, we'd find the pt_regs on the 
    // stack and set rax = 0.
    
    wake_up_new_task(p);
    return pid;
}

pid_t sys_fork(void) {
    return do_fork(0, 0);
}

void sys_exit(int error_code) {
  struct task_struct *curr = get_current();
  
  cpu_cli();
  curr->state = TASK_ZOMBIE;
  
  // TODO: Notify parent, cleanup children
  
  schedule();
  while (1) cpu_hlt();
}

void free_task(struct task_struct *task) {
  if (!task) return;

  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_del(&task->tasks);
  list_del(&task->sibling);
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  if (task->pid >= 0) release_pid(task->pid);
  if (task->thread.fpu) fpu_free(task->thread.fpu);
  if (task->stack) vfree(task->stack);
  
  free_task_struct(task);
}

struct task_struct *alloc_task_struct(void) {
  return kmalloc(sizeof(struct task_struct));
}

void free_task_struct(struct task_struct *task) {
  if (task) kfree(task);
}

void wake_up_new_task(struct task_struct *p) {
  struct rq *rq;
  int cpu = p->cpu;

  if (p->sched_class->select_task_rq) {
    cpu = p->sched_class->select_task_rq(p, cpu, WF_FORK);
  }
  set_task_cpu(p, cpu);
  rq = per_cpu_ptr(runqueues, cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  p->state = TASK_RUNNING;
  activate_task(rq, p);

  if (p->sched_class->check_preempt_curr) {
    p->sched_class->check_preempt_curr(rq, p, WF_FORK);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

struct task_struct *process_spawn(int (*entry)(void *), void *data,
                                  const char *name) {
  struct task_struct *curr = get_current();
  struct task_struct *p = copy_process(0, 0, curr);
  if (!p) return NULL;

  strncpy(p->comm, name, sizeof(p->comm));

  uint64_t *sp = (uint64_t *)((uint8_t *)p->stack + (PAGE_SIZE * 4));
  *(--sp) = (uint64_t)ret_from_kernel_thread;
  *(--sp) = 0;               // rbx
  *(--sp) = 0;               // rbp
  *(--sp) = (uint64_t)entry; // r12
  *(--sp) = (uint64_t)data;  // r13
  *(--sp) = 0;               // r14
  *(--sp) = 0;               // r15

  p->thread.rsp = (uint64_t)sp;

  wake_up_new_task(p);
  return p;
}
EXPORT_SYMBOL(process_spawn);