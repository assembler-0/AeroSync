/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/ic.c
 * @brief IC module driver entrance
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

#include <kernel/fkx/fkx.h>
#include <drivers/apic/pic.h>
#include <drivers/apic/apic.h>

const struct fkx_kernel_api *ic_kapi = NULL;

int ic_mod_init(const struct fkx_kernel_api *api) {
  if (!api) return -1;
  ic_kapi = api;
  ic_kapi->ic_register_controller(apic_get_driver());
  ic_kapi->ic_register_controller(pic_get_driver());
  return 0;
}

FKX_MODULE_DEFINE(
  ic,
  "0.0.1",
  "assembler-0",
  "APIC & PIC interrupt driver module",
  0,
  FKX_IC_CLASS,
  ic_mod_init,
  NULL
);