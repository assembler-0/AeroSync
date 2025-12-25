/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/timer/timer.c
 * @brief timer module entrance
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

#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>
#include <kernel/fkx/fkx.h>

const struct fkx_kernel_api *timer_kapi = NULL;

int timer_mod_init(const struct fkx_kernel_api *api) {
  if (!api) return -1;
  timer_kapi = api;
  timer_kapi->time_register_source(hpet_get_time_source());
  timer_kapi->time_register_source(pit_get_time_source());
  return 0;
}

FKX_MODULE_DEFINE(
  timer,
  "0.0.1",
  "assembler-0",
  "HPET & PIT driver",
  0,
  FKX_TIMER_CLASS,
  timer_mod_init,
  NULL
);