#include <kernel/classes.h>
#include <compiler.h>
#include <arch/x64/cpu.h>
#include <lib/printk.h>

void __exit __noinline __noreturn __sysv_abi
panic_early() {
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg) {
    printk(KERN_EMERG PANIC_CLASS "panic -- not syncing: %s", msg);
    system_hlt();
    __unreachable();
}
