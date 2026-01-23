/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/shutdown.c
 * @brief ACPI Shutdown and Reboot Implementation
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

#include <drivers/acpi/power.h>
#include <lib/printk.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <arch/x86_64/io.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/ic.h>

void acpi_shutdown(void) {
  printk(POWER_CLASS "Preparing for S5 Soft Off...\n");

  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  ic_shutdown_controller();

  uacpi_status ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to prepare for S5: %s\n", uacpi_status_to_string(ret));
    goto rollback;
  }

  printk_disable();

  ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
  if (uacpi_unlikely_error(ret)) {
    printk_enable();
    printk(KERN_ERR POWER_CLASS "Failed to enter S5: %s\n", uacpi_status_to_string(ret));
    goto rollback;
  }

  printk_enable();

rollback:
  /* if we reached here, ACPI sleep failed */
  ic_install();
  restore_irq_flags(flags);
}

void acpi_reboot(void) {
  printk(POWER_CLASS "Attempting ACPI Reboot...\n");

  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  ic_shutdown_controller();

  printk_disable();

  uacpi_status ret = uacpi_reboot();
  if (uacpi_unlikely_error(ret)) {
    printk_enable();
    printk(KERN_ERR POWER_CLASS "ACPI Reboot failed: %s\n", uacpi_status_to_string(ret));
    goto rollback;
  }

  printk_enable();

rollback:
  /* if we reached here, ACPI sleep failed */
  ic_install();
  restore_irq_flags(flags);
}