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
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/sysintf/udm.h>

void acpi_shutdown(void) {
  printk(POWER_CLASS "Preparing for S5 Soft Off...\n");

  uacpi_status ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to prepare for S5: %s\n", uacpi_status_to_string(ret));
    return;
  }

#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  const int k_ret = udm_shutdown_all();
  if (k_ret != 0) {
    printk(KERN_ERR POWER_CLASS "Failed to shutdown devices: %d\n", k_ret)
    return;
  }

  printk_disable();
#endif

  ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to enter S5: %s\n", uacpi_status_to_string(ret));
#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
    printk_enable();
    goto rollback;
#endif
  }

#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
rollback:
  /* if we reached here, ACPI sleep failed */
  udm_restart_all();
  restore_irq_flags(flags);
#endif
}

void acpi_reboot(void) {
  printk(POWER_CLASS "Attempting ACPI Reboot...\n");

#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  const int k_ret = udm_shutdown_all();
  if (k_ret != 0) {
    printk(KERN_ERR POWER_CLASS "Failed to shutdown devices: %d\n", k_ret)
    return;
  }

  printk_disable();
#endif

  uacpi_status ret = uacpi_reboot();
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "ACPI Reboot failed: %s\n", uacpi_status_to_string(ret));
#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
    printk_enable();
    goto rollback;
#endif
  }

#ifdef ACPI_POWER_KERNEL_DEINITIALIZE

  printk_enable();

rollback:
  /* if we reached here, ACPI sleep failed */
  udm_restart_all();
  restore_irq_flags(flags);
#endif
}