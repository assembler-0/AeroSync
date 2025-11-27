#include <compiler.h>
#include <arch/x64/cpu.h>
#include <lib/printk.h>

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg) {
    fprintk(STDERR_FD,"\n\npanic -- not syncing: %s", msg);
    system_hlt();
    __unreachable();
}
