#pragma once

#include <arch/x64/cpu.h>

typedef void (*irq_handler_t)(cpu_regs *regs);

void irq_install_handler(uint8_t vector, irq_handler_t handler);
void irq_uninstall_handler(uint8_t vector);
