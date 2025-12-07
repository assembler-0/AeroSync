#include <kernel/classes.h>
#include <compiler.h>
#include <arch/x64/cpu.h>
#include <lib/printk.h>
#include <arch/x64/exception.h>

void __exit __noinline __noreturn __sysv_abi
panic_early() {
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg) {
    printk(KERN_EMERG PANIC_CLASS "panic - not syncing: %s", msg);
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi
panic_exception(cpu_regs *regs) {
    char exception[256];
    get_exception_as_str(exception, regs->interrupt_number);
    printk(KERN_EMERG PANIC_CLASS "panic - exception - not syncing: %s", exception);
    system_hlt();
    __unreachable();
}