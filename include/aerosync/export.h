#pragma once

#include <aerosync/ksymtab.h>

/**
 * EXPORT_SYMBOL_INTERNAL - Internal helper for symbol export
 */
#define EXPORT_SYMBOL_INTERNAL(sym, lic) \
  static const char __sym_name_##sym[] = #sym; \
    __attribute__((section("ksymtab"), used))   \
    const struct ksymbol __sym_##sym = {       \
    .addr = (uintptr_t)&sym,                    \
    .name = __sym_name_##sym,                  \
    .license = lic                             \
  }

/**
 * EXPORT_SYMBOL - Export a symbol available to all modules
 */
#define EXPORT_SYMBOL(sym) EXPORT_SYMBOL_INTERNAL(sym, KSYM_LICENSE_NONE)

/**
 * EXPORT_SYMBOL_GPL - Export a symbol only to GPL-compatible modules
 */
#define EXPORT_SYMBOL_GPL(sym) EXPORT_SYMBOL_INTERNAL(sym, KSYM_LICENSE_GPL)

/**
 * EXPORT_SYMBOL_MIT - Export a symbol only to MIT-compatible modules
 */
#define EXPORT_SYMBOL_MIT(sym) EXPORT_SYMBOL_INTERNAL(sym, KSYM_LICENSE_MIT)