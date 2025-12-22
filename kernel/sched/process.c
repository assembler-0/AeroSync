/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sched/process.c
 * @brief Process and thread management
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

#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <vsprintf.h>

/*
 * Process/Thread Management
 */

extern void activate_task(struct rq *rq, struct task_struct *p);
extern struct rq per_cpu_runqueues[];
extern struct rq *this_rq(void);

extern char _text_start[];
extern char _text_end[];
extern char _rodata_start[];
extern void ret_from_kernel_thread(void);

extern void vmm_dump_entry(uint64_t pml4_phys, uint64_t virt);
extern uint64_t g_kernel_pml4;

static pid_t next_pid = 1;
static spinlock_t pid_lock = 0;

static pid_t alloc_pid(void) {
  spinlock_lock(&pid_lock);
  pid_t pid = next_pid++;
  spinlock_unlock(&pid_lock);
  return pid;
}

// Defined in switch.S
struct task_struct *__switch_to(struct thread_struct *prev,
                                struct thread_struct *next);

struct task_struct *switch_to(struct task_struct *prev,
                              struct task_struct *next) {
  if (prev == next)
    return prev; // No switch, return current task

  return __switch_to(&prev->thread, &next->thread);
}

// Entry point for new kernel threads
// __used for LTO
void __used kthread_entry_stub(int (*threadfn)(void *data), void *data) {
  cpu_sti();

  threadfn(data);

  sys_exit(0);
}

struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   int nice_value, const char *namefmt, ...) {
  // Allocate task_struct using slab allocator
  struct task_struct *ts = alloc_task_struct();
  if (!ts)
    return NULL;

  memset(ts, 0, sizeof(*ts));

  void *stack = vmalloc(PAGE_SIZE * 2); // 8KB stack
  if (!stack) {
    free_task_struct(ts);
    return NULL;
  }

  ts->stack = stack;
  ts->flags = PF_KTHREAD;
  ts->state = TASK_RUNNING;
  ts->mm = NULL;
  ts->active_mm = &init_mm;

  ts->nice = nice_value;
  // Clamp nice value to valid range
  if (ts->nice < MIN_NICE)
    ts->nice = MIN_NICE;
  if (ts->nice > MAX_NICE)
    ts->nice = MAX_NICE;

  // Calculate load weight based on nice value
  ts->se.load.weight =
      prio_to_weight[ts->nice + 20]; // +20 to map -20..19 to 0..39
  ts->se.vruntime = 0;               // Should inherit or be set fairly

  va_list ap;
  va_start(ap, namefmt);
  vsnprintf(ts->comm, sizeof(ts->comm), namefmt, ap);
  va_end(ap);

  // Initialize list heads
  INIT_LIST_HEAD(&ts->run_list);
  INIT_LIST_HEAD(&ts->tasks);
  INIT_LIST_HEAD(&ts->sibling);
  INIT_LIST_HEAD(&ts->children);

  // Setup Thread Context - stack grows down from end
  uint64_t *sp = (uint64_t *)((uint8_t *)stack + 8192);

  // Simulate the stack frame for __switch_to
  /*
      buffer:
      ret addr -> kthread_entry_stub
      rbx, rbp, r12, r13, r14, r15 -> 0
  */

  *(--sp) = (uint64_t)kthread_entry_stub; // Return address (rip)

  // Registers popped by __switch_to
  *(--sp) = 0; // r15
  *(--sp) = 0; // r14
  *(--sp) = 0; // r13
  *(--sp) = 0; // r12
  *(--sp) = 0; // rbp
  *(--sp) = 0; // rbx

  ts->thread.rsp = (uint64_t)sp;

  // We need to pass arguments to kthread_entry_stub.
  // SysV ABI: rdi = arg1, rsi = arg2.
  // __switch_to doesn't restore RDI/RSI.
  // We need a trampoline helper that moves popped values to RDI/RSI?
  // OR we change __switch_to to also save/restore RDI/RSI (callee saved in
  // Windows, but volatile in SysV?) In SysV, RDI/RSI are caller-saved.

  // SOLUTION: Use a trampoline in assembly that is the "return address".
  // "ret" jumps to ret_from_fork defined in entry.S, which handles args.
  // But simplest way:
  // make r12 = fn, r13 = data (since they are callee saved and restored!)
  // Update kthread_entry_stub to take args from r12/r13? No, it's a C function.

  // Let's modify the stack setup to use a small assembly helper
  // 'fast_kthread_start' that moves r12->rdi, r13->rsi, then calls the C
  // function.

  // Update the stack:
  sp = (uint64_t *)((uint8_t *)stack + 8192);
  *(--sp) = (uint64_t)ret_from_kernel_thread;

  *(--sp) = 0;                  // rbx
  *(--sp) = 0;                  // rbp
  *(--sp) = (uint64_t)threadfn; // r12 -> becomes rdi
  *(--sp) = (uint64_t)data;     // r13 -> becomes rsi
  *(--sp) = 0;                  // r14
  *(--sp) = 0;                  // r15

  ts->thread.rsp = (uint64_t)sp;

  return ts;
}

void kthread_run(struct task_struct *k) {
  if (!k)
    return;

  struct rq *rq = this_rq();
  unsigned long flags = spinlock_lock_irqsave((volatile int *)&rq->lock);

  activate_task(rq, k);

  spinlock_unlock_irqrestore((volatile int *)&rq->lock, flags);
}

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

  if (task->stack) {
    vfree(task->stack);
    task->stack = NULL;
  }

  free_task_struct(task);
}

void wake_up_new_task(struct task_struct *p) {
  p->state = TASK_RUNNING;
  struct rq *rq = &per_cpu_runqueues[p->cpu];

  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);
  activate_task(rq, p);
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
    free_task_struct(ts);
    return NULL;
  }

  // Copy kernel stack
  memcpy(ts->stack, p->stack, PAGE_SIZE * 2);

  // Calculate new stack pointer
  uint64_t stack_offset = (uint64_t)p->thread.rsp - (uint64_t)p->stack;
  ts->thread.rsp = (uint64_t)ts->stack + stack_offset;

  // Handle address space
  if (clone_flags & CLONE_VM) {
    ts->mm = p->mm;
  } else if (p->mm) {
    ts->mm = mm_copy(p->mm);
  } else {
    ts->mm = NULL;
  }

  ts->active_mm = ts->mm ? ts->mm : p->active_mm;

  // Initializations
  INIT_LIST_HEAD(&ts->tasks);
  INIT_LIST_HEAD(&ts->children);
  INIT_LIST_HEAD(&ts->sibling);
  INIT_LIST_HEAD(&ts->run_list);

  // TODO: Add to parent's children list

  return ts;
}

struct task_struct *process_spawn(int (*entry)(void *), void *data,
                                  const char *name) {
  struct task_struct *ts = alloc_task_struct();
  if (!ts)
    return NULL;

  memset(ts, 0, sizeof(*ts));

  // Create its own address space
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

  // Pick the least loaded CPU for initial placement
  int best_cpu = 0;
  unsigned int min_running = 0xFFFFFFFF;
  for (int i = 0; i < smp_get_cpu_count(); i++) {
    if (per_cpu_runqueues[i].nr_running < min_running) {
      min_running = per_cpu_runqueues[i].nr_running;
      best_cpu = i;
    }
  }
  ts->cpu = best_cpu;

  strncpy(ts->comm, name, sizeof(ts->comm));

  // Setup stack for __switch_to returning to ret_from_kernel_thread
  uint64_t *sp = (uint64_t *)((uint8_t *)ts->stack + 8192);
  *(--sp) = (uint64_t)ret_from_kernel_thread;
  *(--sp) = 0;               // rbx
  *(--sp) = 0;               // rbp
  *(--sp) = (uint64_t)entry; // r12 -> becomes rdi
  *(--sp) = (uint64_t)data;  // r13 -> becomes rsi
  *(--sp) = 0;               // r14
  *(--sp) = 0;               // r15

  ts->thread.rsp = (uint64_t)sp;

  // Initialize scheduler entity with proper values
  ts->se.vruntime = 0;  // Start with 0 vruntime for fairness
  ts->se.exec_start_ns = 0; // Will be set when scheduled
  ts->se.sum_exec_runtime = 0;
  ts->se.prev_sum_exec_runtime = 0;
  ts->se.on_rq = 0; // Will be set when enqueued

  // Set default load weight based on nice value
  ts->nice = 0; // Default nice value
  ts->se.load.weight = prio_to_weight[ts->nice + NICE_TO_PRIO_OFFSET];

  // Map initial code? (Simplified: assuming entry is in kernel text which is
  // shared) For a real user process, we would use mm_map_range to load an ELF.

  wake_up_new_task(ts);
  return ts;
}

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
  // Very basic exit
  struct task_struct *curr = get_current();
  curr->state = TASK_ZOMBIE;

  // We cannot free the stack here because we are running on it!
  // The scheduler (finish_task_switch) will handle the cleanup.

  // Deschedule
  schedule();

  // Should never reach here
  while (1)
    ;
}
