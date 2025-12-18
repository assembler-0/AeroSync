#include <arch/x64/cpu.h>
#include <drivers/apic/ic.h>
#include <kernel/panic.h>
#include <kernel/sched/sched.h>

#define IRQ_BASE_VECTOR 32
#define MAX_INTERRUPTS 256

extern void irq_sched_ipi_handler(void);

typedef void (*irq_handler_t)(cpu_regs *regs);
static irq_handler_t irq_handlers[MAX_INTERRUPTS];

void irq_install_handler(uint8_t vector, irq_handler_t handler) {
    irq_handlers[vector] = handler;
}

void irq_uninstall_handler(uint8_t vector) {
    irq_handlers[vector] = NULL;
}

void __used __hot irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31
  if (regs->interrupt_number < IRQ_BASE_VECTOR) {
    panic_exception(regs);
  }

  ic_send_eoi(regs->interrupt_number);

  if (regs->interrupt_number == IRQ_SCHED_IPI_VECTOR + IRQ_BASE_VECTOR) {
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
