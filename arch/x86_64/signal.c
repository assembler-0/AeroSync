/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/signal.c
 * @brief Signal frame for x86_64
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


#include <aerosync/signal.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/entry.h>
#include <aerosync/sched/process.h>
#include <lib/uaccess.h>
#include <lib/string.h>

struct sigcontext {
  uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
  uint64_t rax, rcx, rdx, rsi, rdi;
  uint64_t err, trapno, rip, cs, rflags, rsp, ss;
  uint64_t oldmask;
  uint64_t cr2;
};

struct rt_sigframe {
  void (*pretcode)(void);

  struct sigcontext sc;
};

void arch_setup_sigframe(void *regs_ptr, bool is_syscall, int sig, sigset_t *oldset) {
  struct task_struct *p = current;
  uint64_t rip, rsp, rflags, cs, ss;
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

  if (is_syscall) {
    struct syscall_regs *regs = (struct syscall_regs *) regs_ptr;
    rip = regs->rip;
    rsp = regs->rsp;
    rflags = regs->rflags;
    cs = regs->cs;
    ss = regs->ss;
    rax = regs->rax;
    rbx = regs->rbx;
    rcx = regs->rip; // SYSCALL saves RIP in RCX
    rdx = regs->rdx;
    rsi = regs->rsi;
    rdi = regs->rdi;
    rbp = regs->rbp;
    r8 = regs->r8;
    r9 = regs->r9;
    r10 = regs->r10;
    r11 = regs->r11_dup;
    r12 = regs->r12;
    r13 = regs->r13;
    r14 = regs->r14;
    r15 = regs->r15;
  } else {
    cpu_regs *regs = (cpu_regs *) regs_ptr;
    rip = regs->rip;
    rsp = regs->rsp;
    rflags = regs->rflags;
    cs = regs->cs;
    ss = regs->ss;
    rax = regs->rax;
    rbx = regs->rbx;
    rcx = regs->rcx;
    rdx = regs->rdx;
    rsi = regs->rsi;
    rdi = regs->rdi;
    rbp = regs->rbp;
    r8 = regs->r8;
    r9 = regs->r9;
    r10 = regs->r10;
    r11 = regs->r11;
    r12 = regs->r12;
    r13 = regs->r13;
    r14 = regs->r14;
    r15 = regs->r15;
  }

  /* Align stack to 16 bytes */
  uint64_t frame_rsp = rsp - sizeof(struct rt_sigframe);
  frame_rsp &= ~15UL;

  struct rt_sigframe *frame = (struct rt_sigframe *) frame_rsp;
  struct rt_sigframe kframe;
  memset(&kframe, 0, sizeof(kframe));

  /* Save context */
  kframe.sc.r15 = r15;
  kframe.sc.r14 = r14;
  kframe.sc.r13 = r13;
  kframe.sc.r12 = r12;
  kframe.sc.rbp = rbp;
  kframe.sc.rbx = rbx;
  kframe.sc.r11 = r11;
  kframe.sc.r10 = r10;
  kframe.sc.r9 = r9;
  kframe.sc.r8 = r8;
  kframe.sc.rax = rax;
  kframe.sc.rcx = rcx;
  kframe.sc.rdx = rdx;
  kframe.sc.rsi = rsi;
  kframe.sc.rdi = rdi;
  kframe.sc.rip = rip;
  kframe.sc.cs = cs;
  kframe.sc.rflags = rflags;
  kframe.sc.rsp = rsp;
  kframe.sc.ss = ss;
  kframe.sc.oldmask = *oldset;

  struct k_sigaction *ka = &p->signal->action[sig - 1];

  if (ka->sa.sa_flags & SA_RESTORER) {
    kframe.pretcode = ka->sa.sa_restorer;
  }

  if (copy_to_user(frame, &kframe, sizeof(kframe))) {
    sys_exit(SIGSEGV);
    return;
  }

  /* Set up registers for signal handler */
  if (is_syscall) {
    struct syscall_regs *regs = (struct syscall_regs *) regs_ptr;
    regs->rip = (uint64_t) ka->sa.sa_handler;
    regs->rsp = frame_rsp;
    regs->rdi = sig;
  } else {
    cpu_regs *regs = (cpu_regs *) regs_ptr;
    regs->rip = (uint64_t) ka->sa.sa_handler;
    regs->rsp = frame_rsp;
    regs->rdi = sig;
  }
}

void sys_rt_sigreturn(struct syscall_regs *regs) {
  struct rt_sigframe *frame = (struct rt_sigframe *) regs->rsp;
  struct rt_sigframe kframe;

  if (copy_from_user(&kframe, frame, sizeof(kframe))) {
    sys_exit(SIGSEGV);
    return;
  }

  /* Restore context */
  regs->r15 = kframe.sc.r15;
  regs->r14 = kframe.sc.r14;
  regs->r13 = kframe.sc.r13;
  regs->r12 = kframe.sc.r12;
  regs->rbp = kframe.sc.rbp;
  regs->rbx = kframe.sc.rbx;
  regs->r11_dup = kframe.sc.r11;
  regs->r10 = kframe.sc.r10;
  regs->r9 = kframe.sc.r9;
  regs->r8 = kframe.sc.r8;
  regs->rax = kframe.sc.rax;
  regs->rdx = kframe.sc.rdx;
  regs->rsi = kframe.sc.rsi;
  regs->rdi = kframe.sc.rdi;
  regs->rip = kframe.sc.rip;
  regs->cs = kframe.sc.cs | 3; /* Force user mode */
  regs->rflags = kframe.sc.rflags;
  regs->rsp = kframe.sc.rsp;
  regs->ss = kframe.sc.ss | 3; /* Force user mode */

  current->blocked = kframe.sc.oldmask;
}
