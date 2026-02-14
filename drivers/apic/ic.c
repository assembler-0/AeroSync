/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/apic/ic.c
 * @brief IC module driver entrance
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/fkx/fkx.h>
#include <drivers/apic/pic.h>
#include <drivers/apic/apic.h>
#include <aerosync/sysintf/ic.h>

int ic_mod_init(void) {
  ic_register_controller(apic_get_driver());
  ic_register_controller(pic_get_driver());
  return 0;
}

static const char *ic_deps[] = {"timer", nullptr};

FKX_MODULE_DEFINE(
  ic,
  "0.0.1",
  "assembler-0",
  "APIC & PIC interrupt driver module",
  0,
  FKX_IC_CLASS,
  ic_mod_init,
  ic_deps
);
