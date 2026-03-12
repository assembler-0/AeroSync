/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/drivers/apic/ic.c
 * @brief IC module driver entrance
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/drivers/apic/pic.h>
#include <arch/x86_64/drivers/apic/apic.h>
#include <aerosync/sysintf/ic.h>

int x86_64_ic_register(void) {
  ic_register_controller(apic_get_driver());
  ic_register_controller(pic_get_driver());
  return 0;
}