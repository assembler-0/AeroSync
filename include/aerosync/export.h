#pragma once

#include <aerosync/ksymtab.h>

/**
 * EXPORT_SYMBOL - Export a symbol to the global kernel symbol table
 *
 * This macro places symbol information into a dedicated section that
 * the FKX loader can parse.
 */
#define EXPORT_SYMBOL(sym) \
  static const char __sym_name_##sym[] = #sym; \
    __attribute__((section("ksymtab"), used))   \
    const struct ksymbol __sym_##sym = {       \
    .addr = (uintptr_t)&sym,                    \
    .name = __sym_name_##sym                   \
  }

#define EXPORT_SYMBOL_GPL(sym) EXPORT_SYMBOL(sym)