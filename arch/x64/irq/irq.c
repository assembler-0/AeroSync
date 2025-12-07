#include <arch/x64/cpu.h>
#include <drivers/apic/apic.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>

void irq_common_stub(cpu_regs *regs) {
  // CPU exceptions are vectors 0-31 (inclusive)
  if (regs->interrupt_number <= 31) {
    panic_exception(regs);
  }

  // Hardware IRQs (32+)
  printk(IRQ_CLASS "IRQ %d fired\n", regs->interrupt_number - 32);

  // CRITICAL: Send EOI to APIC to acknowledge the interrupt
  apic_send_eoi();
}