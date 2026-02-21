///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/panic.c
 * @brief builtin kernel panic handler with advanced KDB
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/exception.h>
#include <arch/x86_64/io.h>
#include <aerosync/compiler.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <aerosync/spinlock.h>
#include <aerosync/panic.h>
#include <aerosync/stacktrace.h>
#include <aerosync/version.h>
#include <aerosync/sched/sched.h>
#include <lib/log.h>
#include <lib/string.h>
#include <drivers/acpi/power.h>


static DEFINE_SPINLOCK(panic_lock);
static cpu_regs *kdb_regs = nullptr;

/* ========================================================================
 * Minimal PS/2 Polling Driver (KDB Exclusive)
 * ======================================================================= */

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT 0x01

static char kdb_scancode_map[128] = {
  0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
  0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6',
  '+', '1', '2', '3', '0', '.'
};

static char kdb_shift_map[128] = {
  0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
  0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6',
  '+', '1', '2', '3', '0', '.'
};

static int kdb_shift_pressed = 0;

static void kdb_ps2_drain(void) {
  /* Drain any pending data in the controller to prevent lockups */
  int timeout = 100000;
  while (timeout-- > 0) {
    if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT) {
      (void)inb(PS2_DATA);
    } else {
      break;
    }
  }
}

static char kdb_poll_char(void) {
  static int extended = 0;
  while (1) {
    uint8_t status = inb(PS2_STATUS);
    if (status & PS2_STATUS_OUTPUT) {
      uint8_t sc = inb(PS2_DATA);

      /* Ignore mouse data if it somehow gets in the buffer (bit 5) */
      if (status & 0x20) continue;

      if (sc == 0xE0) {
        extended = 1;
        continue;
      }

      if (sc == 0x2A || sc == 0x36) {
        kdb_shift_pressed = 1;
        extended = 0;
        continue;
      } // Shift down
      
      if (sc == 0xAA || sc == 0xB6) {
        kdb_shift_pressed = 0;
        extended = 0;
        continue;
      } // Shift up

      /* Handle release codes */
      if (sc & 0x80) {
        extended = 0;
        continue; 
      }

      /* Discard extended scancodes for now to keep it simple and safe */
      if (extended) {
        extended = 0;
        continue;
      }

      if (sc < 128) {
        char c = kdb_shift_pressed ? kdb_shift_map[sc] : kdb_scancode_map[sc];
        if (c) return c;
      }
    }
  }
}

static void kdb_gets(char *buf, int max) {
  int i = 0;
  while (i < max - 1) {
    char c = kdb_poll_char();
    if (c == '\n') {
      printk(KERN_RAW "\n");
      buf[i] = 0;
      return;
    } else if (c == '\b') {
      if (i > 0) {
        i--;
        printk(KERN_RAW "\b \b");
      }
    } else {
      buf[i++] = c;
      printk(KERN_RAW "%c", c);
    }
  }
  buf[i] = 0;
  printk(KERN_RAW "\n");
}

/* ========================================================================
 * KDB Diagnostics Logic
 * ======================================================================= */

static void dump_registers(cpu_regs *regs) {
  if (!regs) return;
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

static void kdb_help(void) {
  printk(KERN_RAW "Available commands:\n");
  printk(KERN_RAW "  help        - Show this help\n");
  printk(KERN_RAW "  regs        - Detailed register dump\n");
  printk(KERN_RAW "  bt          - Stack backtrace\n");
  printk(KERN_RAW "  md <addr>   - Memory dump (16 bytes)\n");
  printk(KERN_RAW "  reboot      - Hard reset system\n");
}

static void kdb_cmd_md(char *arg) {
  if (!arg || !*arg) {
    printk(KERN_RAW "Usage: md <hex_addr>\n");
    return;
  }

  uint64_t addr = 0;
  while (*arg && *arg != ' ') {
    char c = *arg++;
    if (c >= '0' && c <= '9') addr = (addr << 4) | (c - '0');
    else if (c >= 'a' && c <= 'f') addr = (addr << 4) | (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') addr = (addr << 4) | (c - 'A' + 10);
  }

  uint8_t *ptr = (uint8_t *) addr;
  printk(KERN_RAW "%016llx: ", addr);
  for (int i = 0; i < 16; i++) {
    printk(KERN_RAW "%02x ", ptr[i]);
  }
  printk(KERN_RAW "| ");
  for (int i = 0; i < 16; i++) {
    char c = ptr[i];
    printk(KERN_RAW "%c", (c >= 32 && c < 127) ? c : '.');
  }
  printk(KERN_RAW "\n");
}

void __exit __noinline __noreturn builtin_kdb_entry_(void) {
  cpu_sti();
  char buf[64];
  printk(iKDB_CLASS "AeroSync iKDB (integrated Kernel debugger). Type 'help' for commands.\n");

  while (1) {
    printk(iKDB_CLASS);
    kdb_gets(buf, sizeof(buf));

    char *cmd = buf;
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "help") == 0) kdb_help();
    else if (strcmp(cmd, "regs") == 0) dump_registers(kdb_regs);
    else if (strcmp(cmd, "bt") == 0) {
      if (kdb_regs) dump_stack_from(kdb_regs->rbp, kdb_regs->rip);
      else dump_stack();
    } else if (strncmp(cmd, "md ", 3) == 0) kdb_cmd_md(cmd + 3);
    else if (strcmp(cmd, "reboot") == 0) {
      acpi_reboot();
      uint8_t good = 0x02;
      while (good & 0x02) good = inb(0x64);
      outb(0xfe, 0x64);
    } else if (*cmd) printk(iKDB_CLASS "Unknown command: %s\n", cmd);
  }
}

/* ========================================================================
 * Panic Handlers
 * ======================================================================= */

static void panic_header(const char *reason) {
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
  printk(KERN_EMERG PANIC_CLASS "                                AeroSync panic\n");
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");

  printk(KERN_EMERG PANIC_CLASS "Reason: %s\n", reason);

#ifdef CONFIG_PANIC_VERBOSE
  struct task_struct *curr = get_current();
  int cpu_id = this_cpu_read(cpu_info.core_id);

  printk(KERN_EMERG PANIC_CLASS "System State:\n");
  printk(KERN_EMERG PANIC_CLASS "  Kernel Version : %s\n", AEROSYNC_VERSION);
  printk(KERN_EMERG PANIC_CLASS "  CPU Core ID    : %d\n", cpu_id);
  if (curr) {
    printk(KERN_EMERG PANIC_CLASS "  Current Task   : %s (pid: %d)\n", curr->comm, curr->pid);
  } else {
    printk(KERN_EMERG PANIC_CLASS "  Current Task   : None (early)\n");
  }
#endif
  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
}

void __exit __noinline __sysv_abi builtin_panic_early_() {}

static cpu_regs panic_internal_regs;

void __exit __noinline __sysv_abi builtin_panic_(const char *msg) {
  log_mark_panic();
  cpu_cli();
  irq_flags_t flags = spinlock_lock_irqsave(&panic_lock);

  panic_header(msg);

#ifdef CONFIG_PANIC_DUMP_REGISTERS
  cpu_regs *regs = &panic_internal_regs;
  __asm__ volatile(
    "mov %%rax, %0\n" "mov %%rbx, %1\n" "mov %%rcx, %2\n" "mov %%rdx, %3\n"
    "mov %%rsi, %4\n" "mov %%rdi, %5\n" "mov %%rbp, %6\n" "mov %%rsp, %7\n"
    "lea (%%rip), %%rax\n" "mov %%rax, %8\n"
    : "=m"(regs->rax), "=m"(regs->rbx), "=m"(regs->rcx), "=m"(regs->rdx),
    "=m"(regs->rsi), "=m"(regs->rdi), "=m"(regs->rbp), "=m"(regs->rsp),
    "=m"(regs->rip)
  );
  __asm__ volatile("pushfq\n popq %0" : "=m"(regs->rflags));
  __asm__ volatile("mov %%cs, %0" : "=m"(regs->cs));
  __asm__ volatile("mov %%ss, %0" : "=m"(regs->ss));

  kdb_regs = regs;
  dump_registers(regs);
#endif

#ifdef CONFIG_PANIC_STACKTRACE
  dump_stack();
#endif

  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
  spinlock_unlock_irqrestore(&panic_lock, flags);
}

void __exit __noinline __sysv_abi builtin_panic_exception_(cpu_regs *regs) {
  log_mark_panic();
  cpu_cli();
  irq_flags_t flags = spinlock_lock_irqsave(&panic_lock);

  char exc_name[128];
  get_exception_as_str(exc_name, regs->interrupt_number);

  char reason[256];
  char except_name[32];
  snprintf(reason, sizeof(reason) - sizeof(except_name), "Exception %s (0x%llx), Error Code: 0x%llx ",
           exc_name, regs->interrupt_number, regs->error_code);

  get_exception_as_str(except_name, regs->interrupt_number);
  strcat(reason, except_name);
  panic_header(reason);
  kdb_regs = regs;
  dump_registers(regs);

#ifdef CONFIG_PANIC_STACKTRACE
  dump_stack_from(regs->rbp, regs->rip);
#endif

  printk(KERN_EMERG PANIC_CLASS "[--------------------------------------------------------------------------------]\n");
  spinlock_unlock_irqrestore(&panic_lock, flags);
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
  .cleanup = builtin_panic_cleanup,
  .kdb = builtin_kdb_entry_
};

const panic_ops_t *get_builtin_panic_ops() {
  return &builtin_panic_ops;
}
