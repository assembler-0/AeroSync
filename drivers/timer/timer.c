/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/timer/timer.c
 * @brief timer module entrance
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/sysintf/time.h>
#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>

int timer_init(void) {
  int err = 0;
  if (time_register_source(hpet_get_time_source()) < 0)
    ++err;
  if (time_register_source(pit_get_time_source()) < 0)
    ++err;
  return err == 0 ? 0 : -EFAULT;
}