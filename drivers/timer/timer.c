/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/timer/timer.c
 * @brief timer module entrance
 * @copyright (C) 2025 assembler-0
 */

#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>
#include <kernel/fkx/fkx.h>
#include <kernel/sysintf/time.h>

int timer_mod_init() {
  time_register_source(hpet_get_time_source());
  time_register_source(pit_get_time_source());
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
