#pragma once

#include <compiler.h>
#include <arch/x64/cpu.h>

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg);
void __exit __noinline __noreturn __sysv_abi
panic_early();
void __exit __noinline __noreturn __sysv_abi
panic_exception(cpu_regs *regs);