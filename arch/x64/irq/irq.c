/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/irq/irq.c
 * @brief Interrupt handling for x64 architecture
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
#include <arch/x64/irq.h>
#include <kernel/sysintf/ic.h>
#include <kernel/panic.h>
#include <kernel/sched/sched.h>

#define IRQ_BASE_VECTOR 32
#define MAX_INTERRUPTS 256

extern void irq_sched_ipi_handler(void);

static irq_handler_t irq_handlers[MAX_INTERRUPTS];

void irq_install_handler(uint8_t vector, irq_handler_t handler) {
  irq_handlers[vector] = handler;
}

void irq_uninstall_handler(uint8_t vector) { irq_handlers[vector] = NULL; }

void __used __hot irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31
  if (regs->interrupt_number < IRQ_BASE_VECTOR) {
    panic_exception(regs);
  }

  // Only send EOI for hardware interrupts and IPIs, not for exceptions
  if (regs->interrupt_number >= IRQ_BASE_VECTOR) {
    ic_send_eoi(regs->interrupt_number);
  }

  if (regs->interrupt_number == IRQ_SCHED_IPI_VECTOR) {
    irq_sched_ipi_handler();
    return;
  }

  if (irq_handlers[regs->interrupt_number]) {
    irq_handlers[regs->interrupt_number](regs);
  }

  if (regs->interrupt_number == IRQ_BASE_VECTOR) {
    scheduler_tick();
    check_preempt();
  }
}
