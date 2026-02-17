#pragma once

#include <compiler.h>
#include <arch/x86_64/cpu.h>
#include <aerosync/sysintf/panic.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

void __exit __noinline __noreturn __sysv_abi
builtin_panic_(const char *msg);
void __exit __noinline __noreturn __sysv_abi
builtin_panic_early_();
void __exit __noinline __noreturn __sysv_abi
builtin_panic_exception_(cpu_regs *regs);
const panic_ops_t *get_builtin_panic_ops(void);

static void __exit __noreturn __noinline __sysv_abi
__aerosync_chk_fail_crit(
  const char *expr,
  const char *file,
  int line
) {
  panic("__aerosync_chk_fail_crit: %s at %s:%d (%s)\n", expr, file, line);
  __unreachable();
}

static void __noinline __sysv_abi
__aerosync_chk_fail_warn(
  const char *expr,
  const char *file,
  int line
) {
  printk( KERN_WARNING KERN_CLASS "__aerosync_chk_fail_warn: %s at %s:%d\n", expr, file, line);
}

#define unmet_function_deprecation(__f) \
  ({  __aerosync_chk_fail_warn(#__f, __FILE__, __LINE__); })

#define unmet_cond_crit(cond) \
  do { if (cond) __aerosync_chk_fail_crit(#cond, __FILE__, __LINE__); } while (0)

#define unmet_cond_crit_else(cond) \
  if (cond) __aerosync_chk_fail_crit(#cond, __FILE__, __LINE__); else

#define unmet_cond_warn(cond) \
  ({ int __r = !!(cond); if (__r) __aerosync_chk_fail_warn(#cond, __FILE__, __LINE__); __r; })

#define unmet_cond_warn_else(cond) \
  if (!!cond) __aerosync_chk_fail_warn(#cond, __FILE__, __LINE__); else

#define unmet_cond_warn_once(cond) \
  ({                                                                                               \
    static int __done; int __r = !!(cond);                                                         \
    if (__r && !__done) { __done = 1; __aerosync_chk_fail_warn(#cond, __FILE__, __LINE__); }        \
    __r;                                                                                           \
  })



