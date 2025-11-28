#pragma once

#include <compiler.h>

void __exit __noinline __noreturn __sysv_abi
panic(const char *msg);
void __exit __noinline __noreturn __sysv_abi
panic_early();