#pragma once

#include <aerosync/compiler.h>
#include <arch/x86_64/cpu.h>
#include <aerosync/panic.h>

void __exit __noinline __sysv_abi
builtin_panic_(const char *msg);
void __exit __noinline __sysv_abi
builtin_panic_early_();
void __exit __noinline __sysv_abi
builtin_panic_exception_(cpu_regs *regs);
const panic_ops_t *get_builtin_panic_ops(void);