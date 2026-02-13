/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/lib/builtins.c
 * @brief Compiler builtin implementations (x86_64)
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <aerosync/export.h>

// x86_64 indirect call thunks for Spectre v2 mitigation
void __x86_indirect_thunk_r11(void) {
  __asm__ volatile (
    "pushq $1f\n\t"
    "jmp *%%r11\n\t"
    "1:\n\t"
    "ret\n\t"
    ::: "memory"
  );
}


EXPORT_SYMBOL(__x86_indirect_thunk_r11);
