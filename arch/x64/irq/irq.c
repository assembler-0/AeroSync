#include <arch/x64/cpu.h>
#include <drivers/apic/ic.h>
#include <kernel/panic.h>
#include <kernel/sched/sched.h>

void __used __hot irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31 (inclusive)
  if (regs->interrupt_number <= 31) {
    panic_exception(regs);
  }

  ic_send_eoi(regs->interrupt_number);

  if (regs->interrupt_number == 32) {
    scheduler_tick();
    // Trigger scheduling immediately on timer tick so that preemption works
    // even when running non-cooperative threads.
    check_preempt();
  }
}