#pragma once

#include <aerosync/types.h>

/**
 * Dump the current kernel stack trace to printk
 */
void dump_stack(void);

/**
 * Dump a stack trace starting from a specific RBP/RIP
 *
 * @param rbp Starting RBP (base pointer)
 * @param rip Starting RIP (instruction pointer)
 */
void dump_stack_from(uint64_t rbp, uint64_t rip);
