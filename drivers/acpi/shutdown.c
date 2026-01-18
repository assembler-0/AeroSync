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

void acpi_shutdown(void) {
  printk(POWER_CLASS "Preparing for S5 Soft Off...\n");

  // 1. Disable Interrupts
  cpu_cli();

  // 2. Shut down APIC/Interrupt subsystems to prevent stray IRQs
  // Assuming apic_get_driver()->shutdown() handles this
  // But we might need to access it directly or trust uACPI to be safe?
  // Usually, we just mask everything.
  // TODO: bring down all core subsystems

  // 3. Prepare uACPI for sleep state S5
  uacpi_status ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to prepare for S5: %s\n", uacpi_status_to_string(ret));
    goto fail;
  }

  // 4. Enter sleep state S5 (this should not return)
  ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);

  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to enter S5: %s\n", uacpi_status_to_string(ret));
  }

fail:
  panic(ACPI_CLASS "ACPI Shutdown failed");
}

void acpi_reboot(void) {
  printk(POWER_CLASS "Attempting ACPI Reboot...\n");

  uacpi_status ret = uacpi_reboot();
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR POWER_CLASS "ACPI Reboot failed: %s\n", uacpi_status_to_string(ret));
  }

  // Fallback to keyboard controller reset
  printk(KERN_NOTICE POWER_CLASS "Fallback: 8042 Reset...\n");
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);

  system_hlt();
}
