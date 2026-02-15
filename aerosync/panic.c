///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/panic.c
 * @brief builtin kernel panic handler with diagnostics
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/exception.h>
#include <compiler.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/panic.h>
#include <aerosync/stacktrace.h>
#include <aerosync/version.h>
#include <aerosync/sched/sched.h>
#include <lib/log.h>
#include <lib/string.h>

static DEFINE_SPINLOCK(panic_lock);

static void dump_registers(cpu_regs *regs) {
  printk(KERN_EMERG PANIC_CLASS "Registers:\n");
  printk(KERN_EMERG PANIC_CLASS "  RAX: %016llx RBX: %016llx RCX: %016llx\n", regs->rax, regs->rbx, regs->rcx);
  printk(KERN_EMERG PANIC_CLASS "  RDX: %016llx RSI: %016llx RDI: %016llx\n", regs->rdx, regs->rsi, regs->rdi);
  printk(KERN_EMERG PANIC_CLASS "  RBP: %016llx R8 : %016llx R9 : %016llx\n", regs->rbp, regs->r8, regs->r9);
  printk(KERN_EMERG PANIC_CLASS "  R10: %016llx R11: %016llx R12: %016llx\n", regs->r10, regs->r11, regs->r12);
  printk(KERN_EMERG PANIC_CLASS "  R13: %016llx R14: %016llx R15: %016llx\n", regs->r13, regs->r14, regs->r15);
  printk(KERN_EMERG PANIC_CLASS "  RIP: %016llx RSP: %016llx RFLAGS: %08llx\n", regs->rip, regs->rsp, regs->rflags);
  printk(KERN_EMERG PANIC_CLASS "  CS : %04llx SS : %04llx\n", regs->cs, regs->ss);

  uint64_t cr0, cr2, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  printk(KERN_EMERG PANIC_CLASS "  CR0: %016llx CR2: %016llx\n", cr0, cr2);
  printk(KERN_EMERG PANIC_CLASS "  CR3: %016llx CR4: %016llx\n", cr3, cr4);
}

static void panic_header(const char *reason) {
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
  printk(KERN_EMERG PANIC_CLASS "                                AeroSync panic\n");
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");

  printk(KERN_EMERG PANIC_CLASS "Reason: %s\n", reason);

#ifdef CONFIG_PANIC_VERBOSE
  struct task_struct *curr = get_current();
  int cpu_id = -1;

  cpu_id = this_cpu_read(cpu_info.core_id);

  printk(KERN_EMERG PANIC_CLASS "System State:\n");
  printk(KERN_EMERG PANIC_CLASS "  Kernel Version : %s\n", AEROSYNC_VERSION);
  printk(KERN_EMERG PANIC_CLASS "  CPU Core ID    : %d\n", cpu_id);
  printk(KERN_EMERG PANIC_CLASS "  Lock on CPU    : %d\n", spinlock_get_cpu(&panic_lock));
  if (curr) {
    printk(KERN_EMERG PANIC_CLASS "  Current Task   : %s (pid: %d)\n", curr->comm, curr->pid);
  } else {
    printk(KERN_EMERG PANIC_CLASS "  Current Task   : None (early)\n");
  }
#endif
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
}

void __exit __noinline __noreturn __sysv_abi builtin_panic_early_() {
  log_mark_panic();
  system_hlt();
  __unreachable();
}

void __exit __noinline __noreturn __sysv_abi builtin_panic_(const char *msg) {
  log_mark_panic();
  spinlock_lock(&panic_lock);

  panic_header(msg);

#ifdef CONFIG_PANIC_DUMP_REGISTERS
  cpu_regs regs;
  __asm__ volatile(
    "mov %%rax, %0\n" "mov %%rbx, %1\n" "mov %%rcx, %2\n" "mov %%rdx, %3\n"
    "mov %%rsi, %4\n" "mov %%rdi, %5\n" "mov %%rbp, %6\n" "mov %%rsp, %7\n"
    "lea (%%rip), %%rax\n" "mov %%rax, %8\n"
    : "=m"(regs.rax), "=m"(regs.rbx), "=m"(regs.rcx), "=m"(regs.rdx),
    "=m"(regs.rsi), "=m"(regs.rdi), "=m"(regs.rbp), "=m"(regs.rsp),
    "=m"(regs.rip)
  );
  __asm__ volatile("pushfq\n popq %0" : "=m"(regs.rflags));
  __asm__ volatile("mov %%cs, %0" : "=m"(regs.cs));
  __asm__ volatile("mov %%ss, %0" : "=m"(regs.ss));
  dump_registers(&regs);
#endif

#ifdef CONFIG_PANIC_STACKTRACE
  dump_stack();
#endif
  printk(KERN_EMERG PANIC_CLASS "[--------------------------- end panic - not syncing ----------------------------]\n");
  system_hlt();
  __unreachable();
}

void __exit __noinline __noreturn __sysv_abi builtin_panic_exception_(cpu_regs *regs) {
  log_mark_panic();
  spinlock_lock(&panic_lock);

  char exc_name[128];
  get_exception_as_str(exc_name, regs->interrupt_number);

  char reason[256];
  snprintf(reason, sizeof(reason), "Exception %s (0x%llx), Error Code: 0x%llx",
           exc_name, regs->interrupt_number, regs->error_code);

  panic_header(reason);

  dump_registers(regs);

#ifdef CONFIG_PANIC_STACKTRACE
  dump_stack_from(regs->rbp, regs->rip);
#endif

  printk(KERN_EMERG PANIC_CLASS "[---------------------------- end panic - exception -----------------------------]\n", exc_name);

  system_hlt();
  __unreachable();
}

static int builtin_panic_init() { return 0; }

static void builtin_panic_cleanup() {
}

static const panic_ops_t builtin_panic_ops = {
  .name = "builtin panic",
  .prio = 100,
  .panic_early = builtin_panic_early_,
  .panic = builtin_panic_,
  .panic_exception = builtin_panic_exception_,
  .init = builtin_panic_init,
  .cleanup = builtin_panic_cleanup
};

const panic_ops_t *get_builtin_panic_ops() {
  return &builtin_panic_ops;
}
