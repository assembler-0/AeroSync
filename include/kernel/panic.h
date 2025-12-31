#pragma once

#include <compiler.h>
#include <arch/x64/cpu.h>
#include <kernel/sysintf/panic.h>

void __exit __noinline __noreturn __sysv_abi
builtin_panic_(const char *msg);
void __exit __noinline __noreturn __sysv_abi
builtin_panic_early_();
void __exit __noinline __noreturn __sysv_abi
builtin_panic_exception_(cpu_regs *regs);
const panic_ops_t *get_builtin_panic_ops(void);