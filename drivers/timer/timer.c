/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/timer/timer.c
 * @brief timer module entrance
 * @copyright (C) 2025-2026 assembler-0
 */

#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/errno.h>

int timer_mod_init(void) {
  int err = 0;
  if (time_register_source(hpet_get_time_source()) < 0) ++err;
  if (time_register_source(pit_get_time_source()) < 0) ++err;
  return err == 0 ? 0 : -EFAULT;
}

FKX_MODULE_DEFINE(
  timer,
  "0.0.1",
  "assembler-0",
  "HPET & PIT driver",
  0,
  FKX_TIMER_CLASS,
  FKX_SUBCLASS_TIMER,
  FKX_NO_REQUIREMENTS,
  timer_mod_init
);
