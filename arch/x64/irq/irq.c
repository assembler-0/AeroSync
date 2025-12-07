#include <arch/x64/cpu.h>
#include <lib/printk.h>
#include <kernel/classes.h>
#include <kernel/panic.h>

void irq_common_stub(cpu_regs *regs) {
    if (regs->interrupt_number < 31) {
        panic_exception(regs);
    } 
}