#pragma once

#include <aerosync/types.h>

/**
 * Kernel Symbol structure
 */
struct ksymbol {
  uintptr_t addr;
  const char *name;
};

/**
 * Lookup a symbol in the global kernel symbol table
 *
 * @param name Name of the symbol
 * @return Address of the symbol or 0 if not found
 */
uintptr_t lookup_ksymbol(const char *name);

/**
 * Register a new symbol in the global kernel symbol table
 *
 * @param addr Address of the symbol
 * @param name Name of the symbol
 * @return 0 on success, error code otherwise
 */
int register_ksymbol(uintptr_t addr, const char *name);