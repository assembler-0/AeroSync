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
 * Lookup a symbol name by its address in the global kernel symbol table
 *
 * @param addr Address to lookup
 * @param offset Optional pointer to store the offset from the symbol start
 * @return Name of the symbol or nullptr if not found
 */
const char *lookup_ksymbol_by_addr(uintptr_t addr, uintptr_t *offset);

/**
 * Register a new symbol in the global kernel symbol table
 *
 * @param addr Address of the symbol
 * @param name Name of the symbol
 * @return 0 on success, error code otherwise
 */
int register_ksymbol(uintptr_t addr, const char *name);

/**
 * Unregister a symbol from the global kernel symbol table
 *
 * @param addr Address of the symbol
 * @return 0 on success, error code otherwise
 */
int unregister_ksymbol(uintptr_t addr);

/**
 * Unregister all symbols within a given address range
 *
 * @param start_addr Start of the address range
 * @param end_addr End of the address range
 */
void unregister_ksymbols_in_range(uintptr_t start_addr, uintptr_t end_addr);

/**
 * Initialize kernel symbol table from ELF image
 *
 * @param kernel_base_addr Address of the loaded kernel ELF file
 */
int ksymtab_init(void *kernel_base_addr);

/**
 * Finalize kernel symbol table (build optimized index).
 * Must be called after memory allocator (vmalloc) is initialized.
 */
int ksymtab_finalize(void);