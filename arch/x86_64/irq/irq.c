/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/irq/irq.c
 * @brief Interrupt handling for x86_64 architecture
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
#include <arch/x86_64/irq.h>
#include <arch/x86_64/mm/tlb.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/panic.h>
#include <aerosync/sched/sched.h>
#include <aerosync/signal.h>
#include <lib/printk.h>

#include <aerosync/timer.h>

#define IRQ_BASE_VECTOR 32
#define MAX_INTERRUPTS 256

extern void irq_sched_ipi_handler(void);

extern void do_page_fault(cpu_regs *regs);

static irq_handler_t irq_handlers[MAX_INTERRUPTS];

void irq_install_handler(uint8_t vector, irq_handler_t handler) {
  irq_handlers[vector] = handler;
}

void irq_uninstall_handler(uint8_t vector) { irq_handlers[vector] = NULL; }

void __used __hot irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31
  if (regs->interrupt_number < IRQ_BASE_VECTOR) {
    if (regs->interrupt_number == 14) {
      do_page_fault(regs);
      goto out_check_signals;
    }

    // If exception happened in user mode, send a signal instead of panicking
    if ((regs->cs & 3) != 0) {
      int sig = 0;
      switch (regs->interrupt_number) {
        case 0: sig = SIGFPE;
          break; // Divide by zero
        case 1: sig = SIGTRAP;
          // debug
        case 3: sig = SIGTRAP;
          break; // Breakpoint
        case 4: sig = SIGSEGV;
          // Overflow
        case 5: sig = SIGSEGV;
          break; // Bound range
        case 6: sig = SIGILL;
          break; // Invalid Opcode
        case 13: sig = SIGSEGV;
          break; // General Protection Fault
        default: sig = SIGILL;
          break;
      }
      send_signal(sig, current);
      goto out_check_signals;
    }

    panic_exception(regs);
  }

  // Only send EOI for hardware interrupts and IPIs, not for exceptions
  if (regs->interrupt_number >= IRQ_BASE_VECTOR && regs->interrupt_number < MAX_INTERRUPTS) {
    ic_send_eoi(regs->interrupt_number);
  }

  if (regs->interrupt_number >= MAX_INTERRUPTS) {
    printk(KERN_ERR "Spurious interrupt with invalid vector: %llu\n", regs->interrupt_number);
    goto out_check_signals;
  }

  if (regs->interrupt_number == IRQ_SCHED_IPI_VECTOR) {
    irq_sched_ipi_handler();
    goto out_check_signals;
  }

  if (regs->interrupt_number == TLB_FLUSH_IPI_VECTOR) {
    tlb_ipi_handler(regs);
    goto out_check_signals;
  }

  if (regs->interrupt_number == CALL_FUNCTION_IPI_VECTOR) {
    smp_call_ipi_handler();
    goto out_check_signals;
  }

  if (irq_handlers[regs->interrupt_number]) {
    irq_handlers[regs->interrupt_number](regs);
  }

  if (regs->interrupt_number == IRQ_BASE_VECTOR) {
    timer_handler();
  }

out_check_signals:
  if ((regs->cs & 3) != 0) {
    do_signal(regs, false);
  }
}
