/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file lib/builtins.c
 * @brief Compiler builtin implementations
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

__uint128_t __udivti3(__uint128_t a, __uint128_t b) {
  __uint128_t q = 0;
  for (int i = 127; i >= 0; i--) {
    if ((b << i) <= a) {
      a -= b << i;
      q |= ((__uint128_t)1 << i);
    }
  }
  return q;
}
