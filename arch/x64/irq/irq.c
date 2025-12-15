#include <arch/x64/cpu.h>
#include <drivers/apic/ic.h>
#include <kernel/panic.h>
#include <kernel/sched/sched.h>

#define IRQ_BASE_VECTOR 32

extern void irq_sched_ipi_handler(void);

void __used __hot irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31
  if (regs->interrupt_number < IRQ_BASE_VECTOR) {
    panic_exception(regs);
  }

  ic_send_eoi(regs->interrupt_number);

  if (regs->interrupt_number == IRQ_SCHED_IPI_VECTOR + IRQ_BASE_VECTOR) {
    irq_sched_ipi_handler();
  }

  if (regs->interrupt_number == IRQ_BASE_VECTOR) {
    scheduler_tick();
    // Trigger scheduling immediately on timer tick so that preemption works
    // even when running non-cooperative threads.
    check_preempt();
  }
}