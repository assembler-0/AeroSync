#include <compiler.h>
#include <drivers/uart/serial.h>

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg) {
    serial_write("\n\npanic -- not syncing: ");
    serial_write(msg);
    system_hlt();
    __unreachable();
}
