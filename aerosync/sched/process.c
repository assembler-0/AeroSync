/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/process.c
 * @brief Process and thread management (Linux-like backend)
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/entry.h>
#include <arch/x86_64/fpu.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/gdt/gdt.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sched/cpumask.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <lib/id_alloc.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <lib/vsprintf.h>
#include <fs/file.h>
#include <aerosync/signal.h>

/*
 * Process/Thread Management
 */

DECLARE_PER_CPU(struct rq, runqueues);

extern void ret_from_kernel_thread(void);
extern void ret_from_user_thread(void);
extern void ret_from_fork(void);

struct ida pid_ida;

/* Global list of all tasks in the system */
LIST_HEAD(task_list);
spinlock_t tasklist_lock = 0;

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
      mm_get(p->mm);
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

  // Setup files
  extern struct files_struct *copy_files(struct files_struct *old_files);
  if (clone_flags & CLONE_FILES) {
      p->files = parent->files;
      atomic_inc(&p->files->count);
  } else {
      p->files = copy_files(parent->files);
      if (!p->files) {
          if (p->mm && p->mm != parent->mm) mm_put(p->mm);
          vfree(p->stack);
          release_pid(p->pid);
          free_task_struct(p);
          return NULL;
      }
  }

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
  p->node_id = parent->node_id;
  p->se.load = parent->se.load;
  cpumask_copy(&p->cpus_allowed, &parent->cpus_allowed);

  // Setup signals
  extern void signal_init_task(struct task_struct *p);
  signal_init_task(p);

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

pid_t do_fork(uint64_t clone_flags, uint64_t stack_start, struct syscall_regs *regs) {
    struct task_struct *curr = get_current();
    struct task_struct *p = copy_process(clone_flags, stack_start, curr);
    if (!p) return -ENOMEM;

    pid_t pid = p->pid;
    
    /* 
     * For fork/clone, we copy the parent's kernel stack content.
     * The syscall_regs are located at the top of the parent's stack.
     */
    uint64_t *parent_sp = (uint64_t *)regs;
    // Calculate how many bytes are used on the stack (from regs to top)
    size_t stack_used = (uint64_t)((uint8_t *)curr->stack + (PAGE_SIZE * 4)) - (uint64_t)parent_sp;
    
    // Position of regs on the child's stack
    uint64_t *child_regs_ptr = (uint64_t *)((uint8_t *)p->stack + (PAGE_SIZE * 4) - stack_used);
    memcpy(child_regs_ptr, parent_sp, stack_used);

    // Child returns 0 in RAX
    struct syscall_regs *child_regs = (struct syscall_regs *)child_regs_ptr;
    child_regs->rax = 0;

    // If stack_start is provided (clone), update it in child_regs
    if (stack_start) {
        child_regs->rsp = stack_start;
    }

    // Setup child's switch_to context
    // We want it to pop r15..rbx and then ret to ret_from_fork
    uint64_t *sp = (uint64_t *)child_regs_ptr;
    *(--sp) = (uint64_t)ret_from_fork;
    *(--sp) = 0;                  // rbx
    *(--sp) = 0;                  // rbp
    *(--sp) = 0;                  // r12
    *(--sp) = 0;                  // r13
    *(--sp) = 0;                  // r14
    *(--sp) = 0;                  // r15

    p->thread.rsp = (uint64_t)sp;
    p->thread.rflags = 0x202;
    
    wake_up_new_task(p);
    return pid;
}

pid_t sys_fork(void) {
    // This is now just a stub if called from kernel, 
    // but proper way is sys_fork_handler in syscall.c
    return -ENOSYS; 
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
  if (task->mm) mm_put(task->mm);
  if (task->stack) vfree(task->stack);

  if (task->files) {
      if (atomic_dec_and_test(&task->files->count)) {
          // Free files_struct and close all files
          for (int i = 0; i < task->files->fdtab.max_fds; i++) {
              if (task->files->fd_array[i]) {
                  fput(task->files->fd_array[i]);
              }
          }
          kfree(task->files);
      }
  }

  if (task->signal) {
      if (--task->signal->count == 0) {
          kfree(task->signal);
      }
  }
  
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

struct task_struct *spawn_user_process_raw(void *data, size_t len, const char *name) {
    struct task_struct *curr = get_current();
    struct task_struct *p = copy_process(0, 0, curr);
    if (!p) return NULL;

    strncpy(p->comm, name, sizeof(p->comm));
    p->flags &= ~PF_KTHREAD;

    /* Create a new MM context for the user process */
    p->mm = mm_create();
    if (!p->mm) {
        free_task(p);
        return NULL;
    }
    p->active_mm = p->mm;

    /* Map code and copy buffer content */
    uint64_t code_addr = 0x400000; // Standard base for simple ELFs/bins
    if (mm_populate_user_range(p->mm, code_addr, len, VM_READ | VM_WRITE | VM_EXEC | VM_USER, data, len) != 0) {
        free_task(p);
        return NULL;
    }

    /* Map user stack */
    uint64_t stack_top = vmm_get_max_user_address() - PAGE_SIZE; // Dynamic canonical stack top
    uint64_t stack_size = PAGE_SIZE * 16;
    uint64_t stack_base = stack_top - stack_size;
    if (mm_populate_user_range(p->mm, stack_base, stack_size, VM_READ | VM_WRITE | VM_USER | VM_STACK, NULL, 0) != 0) {
        free_task(p);
        return NULL;
    }

    /* Setup kernel stack for ret_from_user_thread */
    /* We push a cpu_regs structure onto the kernel stack */
    cpu_regs *regs = (cpu_regs *)((uint8_t *)p->stack + (PAGE_SIZE * 4) - sizeof(cpu_regs));
    memset(regs, 0, sizeof(cpu_regs));

    regs->rip = code_addr;
    regs->rsp = stack_top - 8; // Align stack
    regs->cs = USER_CODE_SELECTOR | 3;
    regs->ss = USER_DATA_SELECTOR | 3;
    regs->rflags = 0x202; // IF=1, bit 1 is reserved and must be 1

    /* segments are not used in 64-bit but we set them for safety/iret frame consistency if needed */
    regs->ds = regs->es = regs->fs = regs->gs = (USER_DATA_SELECTOR | 3);

    /* ret_from_user_thread expects rax to be 'prev' but for the very first switch 
     * it will be passed from __switch_to return.
     * The stack layout should match what ret_from_user_thread expects.
     */
    uint64_t *sp = (uint64_t *)regs;
    *(--sp) = (uint64_t)ret_from_user_thread;
    *(--sp) = 0; // rbx
    *(--sp) = 0; // rbp
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    p->thread.rsp = (uint64_t)sp;

    wake_up_new_task(p);
    return p;
}
EXPORT_SYMBOL(spawn_user_process_raw);