#pragma once

#include <kernel/types.h>

/**
 * Initializes syscall MSRs (STAR, LSTAR, FMASK, EFER.SCE).
 */
void syscall_init(void);
