/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/signal.c
 * @brief standard signals
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

#include <uaccess.h>
#include <aerosync/signal.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sched/process.h>
#include <aerosync/errno.h>
#include <aerosync/panic.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <arch/x86_64/entry.h>
#include <aerosync/classes.h>
#include <linux/container_of.h>

void signal_init_task(struct task_struct *p) {
  p->pending = 0;
  p->blocked = 0;

  if (p->flags & PF_KTHREAD) {
    p->signal = nullptr;
    return;
  }

  if (p->signal == nullptr) {
    p->signal = kmalloc_node(sizeof(struct signal_struct), p->node_id);
    if (!p->signal) {
      panic("Failed to allocate signal_struct");
    }
    memset(p->signal, 0, sizeof(struct signal_struct));
    p->signal->count = 1;
    init_waitqueue_head(&p->signal->wait_chldexit);
  } else {
    p->signal->count++;
  }
}

int send_signal(int sig, struct task_struct *p) {
  if (sig < 1 || sig >= NSIG)
    return -EINVAL;

  if (p->flags & PF_EXITING)
    return -ESRCH;

  p->pending |= sigmask(sig);

  /* Wake up the task if it's sleeping interruptibly */
  if (p->state == TASK_INTERRUPTIBLE) {
    task_wake_up(p);
  }

  return 0;
}

static int next_signal(sigset_t pending, sigset_t blocked) {
  sigset_t ready = pending & ~blocked;
  if (!ready)
    return 0;

  for (int i = 1; i < NSIG; i++) {
    if (ready & sigmask(i))
      return i;
  }
  return 0;
}

void do_signal(void *regs, bool is_syscall) {
  struct task_struct *p = current;

  if (p->flags & PF_KTHREAD)
    return;

  int sig = next_signal(p->pending, p->blocked);
  if (sig == 0)
    return;

  /* Clear the signal from pending */
  p->pending &= ~sigmask(sig);

  struct k_sigaction *ka = &p->signal->action[sig - 1];

  if (ka->sa.sa_handler == SIG_IGN) {
    return;
  }

  if (ka->sa.sa_handler == SIG_DFL) {
    /* Default actions */
    switch (sig) {
      case SIGCHLD:
      case SIGURG:
      case SIGWINCH:
        return;
      case SIGKILL:
      case SIGSTOP:
        /* These cannot be ignored or caught, but here they are handled as DFL */
        // Fallthrough
      case SIGSEGV:
      default:
        printk(KERN_DEBUG SIGNAL_CLASS "task %d terminated by signal %d\n", p->pid, sig);
        sys_exit(sig);
        return;
    }
  }

  /* Handle user-defined signal handler */
  arch_setup_sigframe(regs, is_syscall, sig, &p->blocked);

  /* Block the signal being handled unless SA_NODEFER is set */
  if (!(ka->sa.sa_flags & SA_NODEFER)) {
    p->blocked |= sigmask(sig);
  }

  /* Add the action's mask to blocked signals */
  p->blocked |= ka->sa.sa_mask;
}

/* System Calls */

void sys_rt_sigaction(struct syscall_regs *regs) {
  int sig = (int) regs->rdi;
  const struct sigaction *act = (const struct sigaction *) regs->rsi;
  struct sigaction *oact = (struct sigaction *) regs->rdx;
  size_t sigsetsize = (size_t) regs->r10;

  if (sigsetsize != sizeof(sigset_t)) {
    regs->rax = -EINVAL;
    return;
  }

  if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) {
    regs->rax = -EINVAL;
    return;
  }

  struct signal_struct *s = current->signal;
  if (!s) {
    regs->rax = -EINVAL;
    return;
  }

  if (oact) {
    if (copy_to_user(oact, &s->action[sig - 1].sa, sizeof(struct sigaction))) {
      regs->rax = -EFAULT;
      return;
    }
  }

  if (act) {
    if (copy_from_user(&s->action[sig - 1].sa, act, sizeof(struct sigaction))) {
      regs->rax = -EFAULT;
      return;
    }
  }

  regs->rax = 0;
}

void sys_rt_sigprocmask(struct syscall_regs *regs) {
  int how = (int) regs->rdi;
  const sigset_t *set = (const sigset_t *) regs->rsi;
  sigset_t *oset = (sigset_t *) regs->rdx;
  size_t sigsetsize = (size_t) regs->r10;

  if (sigsetsize != sizeof(sigset_t)) {
    regs->rax = -EINVAL;
    return;
  }

  if (oset) {
    if (copy_to_user(oset, &current->blocked, sizeof(sigset_t))) {
      regs->rax = -EFAULT;
      return;
    }
  }

  if (set) {
    sigset_t newset;
    if (copy_from_user(&newset, set, sizeof(sigset_t))) {
      regs->rax = -EFAULT;
      return;
    }

    /* SIGKILL and SIGSTOP cannot be blocked */
    newset &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

    switch (how) {
      case SIG_BLOCK:
        current->blocked |= newset;
        break;
      case SIG_UNBLOCK:
        current->blocked &= ~newset;
        break;
      case SIG_SETMASK:
        current->blocked = newset;
        break;
      default:
        regs->rax = -EINVAL;
        return;
    }
  }

  regs->rax = 0;
}

void sys_kill(struct syscall_regs *regs) {
  pid_t pid = (pid_t) regs->rdi;
  int sig = (int) regs->rsi;

  if (pid <= 0) {
    /* Simplified: only support positive PIDs for now */
    regs->rax = -ENOSYS;
    return;
  }

  /* Find the task with the given PID */
  struct task_struct *p = nullptr;
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_for_each_entry(p, &task_list, tasks) {
    if (p->pid == pid) {
      break;
    }
  }
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  if (!p || p->pid != pid) {
    regs->rax = -ESRCH;
    return;
  }

  regs->rax = send_signal(sig, p);
}

void sys_tkill(struct syscall_regs *regs) {
  pid_t tid = (pid_t) regs->rdi;
  int sig = (int) regs->rsi;

  struct task_struct *p = nullptr;
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_for_each_entry(p, &task_list, tasks) {
    if (p->pid == tid) {
      break;
    }
  }
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  if (!p || p->pid != tid) {
    regs->rax = -ESRCH;
    return;
  }

  regs->rax = send_signal(sig, p);
}

void sys_tgkill(struct syscall_regs *regs) {
  pid_t tgid = (pid_t) regs->rdi;
  pid_t tid = (pid_t) regs->rsi;
  int sig = (int) regs->rdx;

  struct task_struct *p = nullptr;
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);
  list_for_each_entry(p, &task_list, tasks) {
    if (p->pid == tid && p->tgid == tgid) {
      break;
    }
  }
  spinlock_unlock_irqrestore(&tasklist_lock, flags);

  if (!p || p->pid != tid || p->tgid != tgid) {
    regs->rax = -ESRCH;
    return;
  }

  regs->rax = send_signal(sig, p);
}
