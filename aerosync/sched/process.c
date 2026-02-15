/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/process.c
 * @brief Process and thread management (Linux-like backend)
 * @copyright (C) 2025-2026 assembler-0
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
#include <aerosync/fkx/fkx.h>
#include <aerosync/sched/cpumask.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <lib/id_alloc.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <lib/string.h>
#include <fs/file.h>
#include <aerosync/signal.h>
#include <aerosync/errno.h>
#include <aerosync/kref.h>
#include <fs/fs_struct.h>
#include <linux/container_of.h>
#include <aerosync/resdomain.h>

#include <aerosync/pid_ns.h>

#include <linux/rculist.h>
#include <aerosync/wait.h>

/*
 * Process/Thread Management
 */

#define THREAD_STACK_SIZE (PAGE_SIZE * 4)

/* Caches for fast allocation */
static kmem_cache_t *task_struct_cachep;

/*
 * Kthread Pre-allocation Pool
 *
 * This system maintains a pool of pre-allocated stacks to make kthread_create
 * almost instantaneous. A background worker refills the pool using bulk 
 * allocation to minimize TLB shootdowns.
 */
#define KTHREAD_POOL_TARGET 64
#define KTHREAD_POOL_LOW 16

struct kthread_stack_node {
  void *stack;
  struct kthread_stack_node *next;
};

static struct {
  struct kthread_stack_node *head;
  atomic_t count;
} kthread_stack_pool;

static struct task_struct * __no_cfi __kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, va_list ap);

static void refill_kthread_stack_pool(void) {
  int current_count = atomic_read(&kthread_stack_pool.count);
  if (current_count >= KTHREAD_POOL_TARGET) return;

  int to_alloc = KTHREAD_POOL_TARGET - current_count;
  void *stacks[KTHREAD_POOL_TARGET];
  
  /* Use bulk stack allocator to minimize TLB shootdowns (1 IPI vs 64) */
  int allocated = vmalloc_bulk_stacks(to_alloc, NUMA_NO_NODE, stacks);
  if (allocated <= 0) return;

  for (int i = 0; i < allocated; i++) {
    struct kthread_stack_node *node = kmalloc(sizeof(struct kthread_stack_node));
    if (!node) {
      vfree(stacks[i]);
      continue;
    }
    node->stack = stacks[i];

    /* Push to lock-free pool */
    struct kthread_stack_node *old_head;
    do {
      old_head = kthread_stack_pool.head;
      node->next = old_head;
    } while (atomic64_cmpxchg((atomic64_t *)&kthread_stack_pool.head, (long)old_head, (long)node) != (long)old_head);
    
    atomic_inc(&kthread_stack_pool.count);
  }
}

static DECLARE_WAIT_QUEUE_HEAD(kthread_pool_wait);

static int kthread_pool_worker_fn(void *unused) {
  (void)unused;
  for (;;) {
    wait_event_interruptible(kthread_pool_wait, atomic_read(&kthread_stack_pool.count) < KTHREAD_POOL_LOW);
    refill_kthread_stack_pool();
  }
  return 0;
}

static void *pop_stack_from_pool(void) {
  struct kthread_stack_node *node;
  struct kthread_stack_node *next;

  do {
    node = kthread_stack_pool.head;
    if (!node) return nullptr;
    next = node->next;
  } while (atomic64_cmpxchg((atomic64_t *)&kthread_stack_pool.head, (long)node, (long)next) != (long)node);

  void *stack = node->stack;
  kfree(node);
  int remaining = atomic_dec_return(&kthread_stack_pool.count);
  
  if (remaining < KTHREAD_POOL_LOW) {
    wake_up_interruptible(&kthread_pool_wait);
  }
  
  return stack;
}

/* Per-CPU stack pool to avoid vmalloc overhead for kthreads */
#define STACK_POOL_SIZE 8
struct stack_pool {
  void *stacks[STACK_POOL_SIZE];
  int count;
  spinlock_t lock;
};

DEFINE_PER_CPU(struct stack_pool, kstack_pools);

static void *alloc_kstack(int node) {
  struct stack_pool *pool = this_cpu_ptr(kstack_pools);
  void *stack = nullptr;

  irq_flags_t flags = spinlock_lock_irqsave(&pool->lock);
  if (pool->count > 0) {
    stack = pool->stacks[--pool->count];
    spinlock_unlock_irqrestore(&pool->lock, flags);
    return stack;
  }
  
  /* Pool empty, try bulk refill to avoid multiple TLB shootdowns */
  void *new_stacks[STACK_POOL_SIZE];
  int allocated = vmalloc_bulk_stacks(STACK_POOL_SIZE, node, new_stacks);
  
  if (allocated > 0) {
    stack = new_stacks[0];
    for (int i = 1; i < allocated; i++) {
      pool->stacks[pool->count++] = new_stacks[i];
    }
    spinlock_unlock_irqrestore(&pool->lock, flags);
    return stack;
  }
  
  spinlock_unlock_irqrestore(&pool->lock, flags);

  /* Fallback to single allocation if bulk fails */
  return vmalloc_node_stack(THREAD_STACK_SIZE, node);
}

static void free_kstack(void *stack) {
  if (!stack) return;
  struct stack_pool *pool = this_cpu_ptr(kstack_pools);

  irq_flags_t flags = spinlock_lock_irqsave(&pool->lock);
  if (pool->count < STACK_POOL_SIZE) {
    pool->stacks[pool->count++] = stack;
    spinlock_unlock_irqrestore(&pool->lock, flags);
  } else {
    spinlock_unlock_irqrestore(&pool->lock, flags);
    vfree(stack);
  }
}

/* Async kthread creation worker */
struct kthread_create_info {
  int (*threadfn)(void *data);
  void *data;
  char namefmt[64];
  struct list_head list;
  struct wait_queue_head done;
  struct task_struct *result;
};

static LIST_HEAD(kthread_create_list);
static DEFINE_SPINLOCK(kthread_create_lock);
static DECLARE_WAIT_QUEUE_HEAD(kthread_create_wait);

static int kthreadd(void *unused) {
  (void)unused;
  for (;;) {
    wait_event_interruptible(kthread_create_wait, !list_empty(&kthread_create_list));

    irq_flags_t flags = spinlock_lock_irqsave(&kthread_create_lock);
    while (!list_empty(&kthread_create_list)) {
      struct kthread_create_info *info = list_first_entry(&kthread_create_list, struct kthread_create_info, list);
      list_del(&info->list);
      spinlock_unlock_irqrestore(&kthread_create_lock, flags);

      info->result = kthread_create(info->threadfn, info->data, info->namefmt);
      if (info->result) {
        kthread_run(info->result);
      }

      wake_up_all(&info->done);

      flags = spinlock_lock_irqsave(&kthread_create_lock);
    }
    spinlock_unlock_irqrestore(&kthread_create_lock, flags);
  }
  return 0;
}

void kthread_init(void) {
  task_struct_cachep = kmem_cache_create("task_struct", sizeof(struct task_struct), 0, SLAB_HWCACHE_ALIGN);

  int cpu;
  for_each_possible_cpu(cpu) {
    struct stack_pool *pool = per_cpu_ptr(kstack_pools, cpu);
    spinlock_init(&pool->lock);
    pool->count = 0;
  }

  /* Initialize Kthread Stack Pool */
  kthread_stack_pool.head = nullptr;
  atomic_set(&kthread_stack_pool.count, 0);

  /* Start pool worker - use __kthread_create directly as pool is not ready */
  kthread_run(__kthread_create(kthread_pool_worker_fn, nullptr, "kthread_pool", (va_list){0}));

  kthread_run(kthread_create(kthreadd, nullptr, "kthreadd"));
}

DECLARE_PER_CPU(struct rq, runqueues);

extern void ret_from_kernel_thread(void);

extern void ret_from_user_thread(void);

extern void ret_from_fork(void);

struct ida pid_ida;

/* Global list of all tasks in the system - RCU protected */
struct list_head task_list;
spinlock_t tasklist_lock;

static void free_task_rcu(struct rcu_head *rcu) {
  struct task_struct *task = container_of(rcu, struct task_struct, rcu);
  free_task_struct(task);
}

struct pid_namespace init_pid_ns = {
  .kref = KREF_INIT(2), /* 2 refs: one for init_task, one for being permanent */
  .parent = nullptr,
  .level = 0,
  .child_reaper = nullptr,
};

void pid_allocator_init(void) {
  /* Initialize global task list and its lock at runtime */
  INIT_LIST_HEAD(&task_list);
  spinlock_init(&tasklist_lock);

  ida_init(&pid_ida, 32768);
  ida_alloc(&pid_ida); // Allocate 0 for idle/init

  ida_init(&init_pid_ns.pid_ida, 32768);
  ida_alloc(&init_pid_ns.pid_ida); /* Reserve 0 */
}

struct pid_namespace *create_pid_namespace(struct pid_namespace *parent) {
  struct pid_namespace *ns = kzalloc(sizeof(struct pid_namespace));
  if (!ns) return nullptr;

  kref_init(&ns->kref);
  ns->parent = parent; /* TODO: get_pid_ns(parent) */
  ns->level = parent ? parent->level + 1 : 0;
  ida_init(&ns->pid_ida, 32768);
  ida_alloc(&ns->pid_ida); /* Reserve 0 */

  return ns;
}

static void free_pid_ns(struct kref *kref) {
  struct pid_namespace *ns = container_of(kref, struct pid_namespace, kref);
  /* TODO: clean up parent ref */
  kfree(ns);
}

void put_pid_ns(struct pid_namespace *ns) {
  if (ns && ns != &init_pid_ns)
    kref_put(&ns->kref, free_pid_ns);
}

static inline void get_pid_ns(struct pid_namespace *ns) {
  if (ns) kref_get(&ns->kref);
}

pid_t pid_ns_alloc(struct pid_namespace *ns) {
  return ida_alloc(&ns->pid_ida);
}

void pid_ns_free(struct pid_namespace *ns, pid_t pid) {
  ida_free(&ns->pid_ida, pid);
}

static pid_t alloc_pid_for_task(struct task_struct *task, struct pid_namespace *ns) {
  /* For now, we only allocate in the active namespace.
     A full implementation would allocate in all parent namespaces. */
  return pid_ns_alloc(ns);
}

static void release_pid(pid_t pid) { ida_free(&pid_ida, pid); }

// Defined in switch.asm
struct task_struct * __no_cfi __switch_to(struct thread_struct *prev,
                                struct thread_struct *next);

struct task_struct * __no_cfi switch_to(struct task_struct *prev,
                              struct task_struct *next) {
  if (prev == next)
    return prev;

  return __switch_to(&prev->thread, &next->thread);
}

// Entry point for new kernel threads
void __no_cfi __used kthread_entry_stub(int (*threadfn)(void *data), void *data) {
  cpu_sti(); // Enable interrupts as we are starting a fresh thread
  const int ret = threadfn(data);
  sys_exit(ret);
}

/*
 * copy_process - The core of fork/clone/kthread_create.
 * Creates a new task and copies/shares resources from the parent.
 */
struct task_struct *copy_process(uint64_t clone_flags,
                                 uint64_t stack_start,
                                 struct task_struct *parent) {
  struct task_struct *p;

  if (parent && resdomain_can_fork(parent->rd) < 0)
    return nullptr;

  p = alloc_task_struct();
  if (!p)
    return nullptr;

  memset(p, 0, sizeof(*p));

  /* Namespace handling */
  if (clone_flags & CLONE_NEWPID) {
    if (!parent) {
      free_task_struct(p);
      return nullptr;
    }
    p->nsproxy = create_pid_namespace(parent->nsproxy);
    if (!p->nsproxy) {
      free_task_struct(p);
      return nullptr;
    }
  } else {
    if (parent) {
      p->nsproxy = parent->nsproxy;
      get_pid_ns(p->nsproxy);
    } else {
      p->nsproxy = &init_pid_ns;
      get_pid_ns(&init_pid_ns);
    }
  }

  p->pid = alloc_pid_for_task(p, p->nsproxy);
  if (p->pid < 0) {
    put_pid_ns(p->nsproxy);
    free_task_struct(p);
    return nullptr;
  }

  if (p->nsproxy->child_reaper == nullptr) {
    p->nsproxy->child_reaper = p;
  }

  // Allocate kernel stack from pool or vmalloc
  if (clone_flags & CLONE_KSTACK) {
    p->stack = (void *)stack_start;
  } else {
    p->stack = alloc_kstack(p->node_id);
  }

  if (!p->stack) {
    release_pid(p->pid);
    free_task_struct(p);
    return nullptr;
  }

  // Initialize basic fields
  p->state = TASK_RUNNING;
  p->cpu = (int) smp_get_id();
  p->flags = 0;
  p->preempt_count = 0;
  p->parent = parent;

  INIT_LIST_HEAD(&p->tasks);
  INIT_LIST_HEAD(&p->children);
  INIT_LIST_HEAD(&p->sibling);
  INIT_LIST_HEAD(&p->run_list);

  /* PI initialization */
  spinlock_init(&p->pi_lock);
  p->pi_blocked_on = nullptr;
  INIT_LIST_HEAD(&p->pi_waiters);
  INIT_LIST_HEAD(&p->pi_list);

  // Setup memory management
  if (!parent) return nullptr;

  if (clone_flags & CLONE_VM) {
    p->mm = parent->mm;
    mm_get(p->mm);
  } else if (parent->mm) {
    p->mm = mm_copy(parent->mm);
    if (!p->mm) {
      free_kstack(p->stack);
      release_pid(p->pid);
      free_task_struct(p);
      return nullptr;
    }
  }
  p->active_mm = p->mm ? p->mm : parent->active_mm;

  /* Initialize ResDomain for MM */
  if (p->mm && p->mm != parent->mm) {
    p->mm->rd = p->rd;
    resdomain_get(p->mm->rd);
  }

  // Setup files
  extern struct files_struct *copy_files(struct files_struct *old_files);
  if (clone_flags & CLONE_FILES) {
    p->files = parent->files;
    atomic_inc(&p->files->count);
  } else {
    p->files = copy_files(parent->files);
    if (!p->files) {
      if (p->mm && p->mm != parent->mm) mm_put(p->mm);
      free_kstack(p->stack);
      release_pid(p->pid);
      free_task_struct(p);
      return nullptr;
    }
  }

  // Setup fs_struct
  if (clone_flags & CLONE_FS) {
    p->fs = parent->fs;
    if (p->fs) atomic_inc(&p->fs->count);
  } else {
    p->fs = copy_fs_struct(parent->fs);
  }

  // Setup Resource Domain
  resdomain_task_init(p, parent);

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
  p->normal_prio = parent->normal_prio;
  p->prio = parent->prio;
  p->rt_priority = parent->rt_priority;
  p->nice = parent->nice;
  p->node_id = parent->node_id;
  p->se.load = parent->se.load;
  cpumask_copy(&p->cpus_allowed, &parent->cpus_allowed);

  // Setup signals
  signal_init_task(p);

  // Link into global lists
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_add_tail_rcu(&p->tasks, &task_list);
  list_add_tail(&p->sibling, &parent->children);
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  return p;
}

struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...) {
  struct task_struct *curr = get_current();
  void *stack = pop_stack_from_pool();
  struct task_struct *p;
  va_list ap;

  va_start(ap, namefmt);
  if (stack) {
    p = copy_process(CLONE_VM | CLONE_KSTACK, (uint64_t)stack, curr);
  } else {
    p = copy_process(CLONE_VM, 0, curr);
  }

  if (!p) {
    va_end(ap);
    return nullptr;
  }

  p->flags |= PF_KTHREAD;
  p->mm = nullptr;

  if (p->sched_class == &idle_sched_class || !p->sched_class) {
    p->sched_class = &fair_sched_class;
    p->prio = DEFAULT_PRIO;
    p->static_prio = DEFAULT_PRIO;
    p->normal_prio = DEFAULT_PRIO;
    p->se.load.weight = NICE_0_LOAD;
    p->se.load.inv_weight = 0;
  }

  vsnprintf(p->comm, sizeof(p->comm), namefmt, ap);
  va_end(ap);

  // Setup stack for ret_from_kernel_thread
  uint64_t *sp = (uint64_t *) ((uint8_t *) p->stack + (PAGE_SIZE * 4));
  *(--sp) = (uint64_t) ret_from_kernel_thread;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = (uint64_t) threadfn; // r12
  *(--sp) = (uint64_t) data; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;
  p->thread.rflags = 0x202;

  return p;
}

struct task_struct * __no_cfi __kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, va_list ap) {
  struct task_struct *curr = get_current();

  struct task_struct *p = copy_process(CLONE_VM, 0, curr);
  if (!p) return nullptr;

  p->flags |= PF_KTHREAD;
  p->mm = nullptr;

  if (p->sched_class == &idle_sched_class || !p->sched_class) {
    p->sched_class = &fair_sched_class;
    p->prio = DEFAULT_PRIO;
    p->static_prio = DEFAULT_PRIO;
    p->normal_prio = DEFAULT_PRIO;
    p->se.load.weight = NICE_0_LOAD;
    p->se.load.inv_weight = 0;
  }

  vsnprintf(p->comm, sizeof(p->comm), namefmt, ap);

  // Setup stack for ret_from_kernel_thread
  uint64_t *sp = (uint64_t *) ((uint8_t *) p->stack + (PAGE_SIZE * 4));
  *(--sp) = (uint64_t) ret_from_kernel_thread;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = (uint64_t) threadfn; // r12
  *(--sp) = (uint64_t) data; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;
  p->thread.rflags = 0x202;

  return p;
}

EXPORT_SYMBOL(kthread_create);

void __no_cfi kthread_run(struct task_struct *k) {
  if (k) wake_up_new_task(k);
}

EXPORT_SYMBOL(kthread_run);

pid_t __no_cfi do_fork(uint64_t clone_flags, uint64_t stack_start, struct syscall_regs *regs) {
  struct task_struct *curr = get_current();
  struct task_struct *p = copy_process(clone_flags, stack_start, curr);
  if (!p) return -ENOMEM;

  pid_t pid = p->pid;

  /*
   * For fork/clone, we copy the parent's kernel stack content.
   * The syscall_regs are located at the top of the parent's stack.
   */
  uint64_t *parent_sp = (uint64_t *) regs;
  // Calculate how many bytes are used on the stack (from regs to top)
  size_t stack_used = (uint64_t) ((uint8_t *) curr->stack + (PAGE_SIZE * 4)) - (uint64_t) parent_sp;

  // Position of regs on the child's stack
  uint64_t *child_regs_ptr = (uint64_t *) ((uint8_t *) p->stack + (PAGE_SIZE * 4) - stack_used);
  memcpy(child_regs_ptr, parent_sp, stack_used);

  // Child returns 0 in RAX
  struct syscall_regs *child_regs = (struct syscall_regs *) child_regs_ptr;
  child_regs->rax = 0;

  // If stack_start is provided (clone), update it in child_regs
  if (stack_start) {
    child_regs->rsp = stack_start;
  }

  // Setup child's switch_to context
  // We want it to pop r15..rbx and then ret to ret_from_fork
  uint64_t *sp = (uint64_t *) child_regs_ptr;
  *(--sp) = (uint64_t) ret_from_fork;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = 0; // r12
  *(--sp) = 0; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;
  p->thread.rflags = 0x202;

  wake_up_new_task(p);
  return pid;
}

pid_t sys_fork(void) {
  // This is now just a stub if called from kernel,
  // but proper way is sys_fork_handler in syscall.c
  return -ENOSYS;
}

static void reparent_children(struct task_struct *parent) {
  struct task_struct *child, *tmp;
  struct task_struct *reaper = parent->nsproxy->child_reaper;

  if (reaper == parent) {
    /* If the reaper itself is exiting, move children to global init or parent reaper */
    if (parent->nsproxy->parent)
      reaper = parent->nsproxy->parent->child_reaper;
    else
      reaper = find_task_by_pid(1); /* Fallback to global init */
  }

  if (!reaper || reaper == parent) return;

  list_for_each_entry_safe(child, tmp, &parent->children, sibling) {
    list_del(&child->sibling);
    child->parent = reaper;
    list_add_tail(&child->sibling, &reaper->children);
    
    if (child->state == TASK_ZOMBIE) {
        /* If child was already zombie, notify the new parent */
        send_signal(SIGCHLD, reaper);
    }
  }
}

void sys_exit(int error_code) {
  struct task_struct *curr = get_current();

  /* 1. Mark as exiting to prevent further allocations/interrupts handling */
  curr->flags |= PF_EXITING;
  curr->exit_code = error_code;

  /* 2. Release Memory Management context */
  if (curr->mm) {
    mm_put(curr->mm);
    curr->mm = nullptr;
  }

  /* 3. Close all open files */
  if (curr->files) {
    if (atomic_dec_and_test(&curr->files->count)) {
      for (int i = 0; i < curr->files->fdtab.max_fds; i++) {
        if (curr->files->fd_array[i]) {
          fput(curr->files->fd_array[i]);
          curr->files->fd_array[i] = nullptr;
        }
      }
      kfree(curr->files);
    }
    curr->files = nullptr;
  }

  /* 4. Release fs_struct */
  if (curr->fs) {
    free_fs_struct(curr->fs);
    curr->fs = nullptr;
  }

  /* 5. Handle process hierarchy */
  spinlock_lock(&tasklist_lock);
  
  /* Reparent my children */
  reparent_children(curr);

  /* 6. Mark as Zombie */
  curr->state = TASK_ZOMBIE;

  /* 7. Notify parent */
  if (curr->parent) {
    send_signal(SIGCHLD, curr->parent);
  }

  spinlock_unlock(&tasklist_lock);

  /* 8. Final Reschedule */
  cpu_cli();
  schedule();

  /* Should never reach here */
  while (1)
    cpu_hlt();
}

void free_task(struct task_struct *task) {
  if (!task) return;

  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_del_rcu(&task->tasks);
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

  if (task->fs) {
    free_fs_struct(task->fs);
  }

  if (task->rd) {
    resdomain_task_exit(task);
  }

  if (task->nsproxy) {
    pid_ns_free(task->nsproxy, task->pid);
    put_pid_ns(task->nsproxy);
  }

  if (task->signal) {
    if (--task->signal->count == 0) {
      kfree(task->signal);
    }
  }

  /* Release the memory through RCU to protect readers */
  call_rcu(&task->rcu, free_task_rcu);
}

struct task_struct *alloc_task_struct(void) {
  struct task_struct *curr = get_current();
  int nid = curr ? curr->node_id : this_node();
  if (likely(task_struct_cachep))
    return kmem_cache_alloc_node(task_struct_cachep, nid);
  return kzalloc_node(sizeof(struct task_struct), nid);
}

void free_task_struct(struct task_struct *task) {
  if (task) {
    if (likely(task_struct_cachep))
      kmem_cache_free(task_struct_cachep, task);
    else
      kfree(task);
  }
}

struct task_struct *find_task_by_pid(pid_t pid) {
  struct task_struct *task;
  rcu_read_lock();
  list_for_each_entry_rcu(task, &task_list, tasks) {
    if (task->pid == pid) {
      rcu_read_unlock();
      return task;
    }
  }
  rcu_read_unlock();
  return nullptr;
}

EXPORT_SYMBOL(find_task_by_pid);

void __no_cfi wake_up_new_task(struct task_struct *p) {
  struct rq *rq;
  int cpu = p->cpu;

  if (p->sched_class->select_task_rq) {
    cpu = p->sched_class->select_task_rq(p, cpu, WF_FORK);
  }
  set_task_cpu(p, cpu);
  rq = per_cpu_ptr(runqueues, cpu);
  irq_flags_t flags = spinlock_lock_irqsave(&rq->lock);

  p->state = TASK_RUNNING;
  activate_task(rq, p, ENQUEUE_WAKEUP);

  if (p->sched_class->check_preempt_curr) {
    p->sched_class->check_preempt_curr(rq, p, WF_FORK);
  }

  spinlock_unlock_irqrestore(&rq->lock, flags);
}

struct task_struct *process_spawn(int (*entry)(void *), void *data,
                                  const char *name) {
  struct task_struct *curr = get_current();
  struct task_struct *p = copy_process(0, 0, curr);
  if (!p) return nullptr;

  strncpy(p->comm, name, sizeof(p->comm));

  uint64_t *sp = (uint64_t *) ((uint8_t *) p->stack + (PAGE_SIZE * 4));
  *(--sp) = (uint64_t) ret_from_kernel_thread;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = (uint64_t) entry; // r12
  *(--sp) = (uint64_t) data; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;
  wake_up_new_task(p);

  return p;
}

EXPORT_SYMBOL(process_spawn);

int do_execve(const char *filename, char **argv, char **envp) {
  struct file *file = vfs_open(filename, O_RDONLY, 0);
  if (!file) return -ENOENT;

  int retval = do_execve_file(file, filename, argv, envp);
  vfs_close(file);

  return retval;
}

EXPORT_SYMBOL(do_execve);

int run_init_process(const char *init_filename) {
  return do_execve(init_filename, nullptr, nullptr);
}

EXPORT_SYMBOL(run_init_process);

#ifdef CONFIG_UNSAFE_USER_TASK_SPAWN
struct task_struct * __deprecated spawn_user_process_raw(void *data, size_t len, const char *name) {
  unmet_function_deprecation(spawn_user_process_raw);

  struct task_struct *curr = get_current();
  struct task_struct *p = copy_process(0, 0, curr);
  if (!p) return nullptr;

  strncpy(p->comm, name, sizeof(p->comm));
  p->flags &= ~PF_KTHREAD;

  /* Create a new MM context for the user process */
  p->mm = mm_create();
  if (!p->mm) {
    free_task(p);
    return nullptr;
  }
  p->active_mm = p->mm;

  /* Map code and copy buffer content */
  uint64_t code_addr = 0x400000; // Standard base for simple ELFs/bins
  if (mm_populate_user_range(p->mm, code_addr, len, VM_READ | VM_WRITE | VM_EXEC | VM_USER, data, len) != 0) {
    free_task(p);
    return nullptr;
  }

  /* Map user stack */
  uint64_t stack_top = vmm_get_max_user_address() - PAGE_SIZE; // Dynamic canonical stack top
  uint64_t stack_size = PAGE_SIZE * 16;
  uint64_t stack_base = stack_top - stack_size;
  if (mm_populate_user_range(p->mm, stack_base, stack_size, VM_READ | VM_WRITE | VM_USER | VM_STACK, nullptr, 0) != 0) {
    free_task(p);
    return nullptr;
  }

  /* Setup kernel stack for ret_from_user_thread */
  /* We push a cpu_regs structure onto the kernel stack */
  cpu_regs *regs = (cpu_regs *) ((uint8_t *) p->stack + (PAGE_SIZE * 4) - sizeof(cpu_regs));
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
  uint64_t *sp = (uint64_t *) regs;
  *(--sp) = (uint64_t) ret_from_user_thread;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = 0; // r12
  *(--sp) = 0; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;

  wake_up_new_task(p);
  return p;
}

EXPORT_SYMBOL(spawn_user_process_raw);
#endif
