#pragma once

#include <aerosync/types.h>

void get_exception_as_str(char* buff, uint32_t num);

struct exception_table_entry {
    int32_t insn;
    int32_t fixup;
};

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];

/**
 * Search the exception table for a fixup address.
 * @param addr The faulting instruction pointer
 * @return The fixup address, or 0 if not found
 */
uint64_t search_exception_table(uint64_t addr);

#define EX_TABLE_ENTRY(insn, fixup) \
    ".section __ex_table,\"a\"\n"    \
    ".align 4\n"                     \
    ".long (" insn ") - .\n"         \
    ".long (" fixup ") - .\n"        \
    ".previous\n"
