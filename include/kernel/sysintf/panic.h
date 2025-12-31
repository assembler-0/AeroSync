#pragma once

#include <compiler.h>
#include <kernel/types.h>

typedef void (*panic_regs)(cpu_regs *regs) __noreturn __sysv_abi;
typedef void (*panic_msg)(const char *msg) __noreturn __sysv_abi;
typedef void (*panic_simple)(void) __noreturn __sysv_abi;
typedef int  (*panic_init)(void);
typedef void (*panic_cleanup)(void);

typedef struct {
  char *name;
  uint32_t prio;
  panic_init init;
  panic_cleanup cleanup;
  panic_msg panic;
  panic_regs panic_exception;
  panic_simple panic_early;
} panic_ops_t;

void panic_register_handler(const panic_ops_t *ops);
void panic_switch_handler(const char *name);
void panic_handler_install();

void __exit __noinline __noreturn __sysv_abi panic_exception(cpu_regs *regs);
void __exit __noinline __noreturn __sysv_abi panic_early();
void __exit __noinline __noreturn __sysv_abi panic(const char *msg);