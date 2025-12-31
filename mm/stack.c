/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/stack.c
 * @brief Stack protection implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <compiler.h>
#include <kernel/panic.h>
#include <kernel/classes.h>
#include <mm/stack.h>
#include <kernel/fkx/fkx.h>

uint64_t __stack_chk_guard = STACK_CANARY_VALUE;

void __exit __noreturn __stack_chk_fail(void) {
    panic(STACK_CLASS "Stack overflow");
}
EXPORT_SYMBOL(__stack_chk_fail);