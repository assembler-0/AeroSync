#include <compiler.h>
#include <kernel/panic.h>
#include <kernel/classes.h>
#include <mm/stack.h>

uint64_t __stack_chk_guard = STACK_CANARY_VALUE;

void __exit __noreturn __stack_chk_fail(void) {
    panic(STACK_CLASS "Stack overflow");
}