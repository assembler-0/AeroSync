/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/power.c
 * @brief ACPI Power Button and Sleep Button handling
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
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <uacpi/event.h>
#include <uacpi/uacpi.h>

static uacpi_interrupt_ret handle_power_button(uacpi_handle ctx) {
  (void) ctx;
  acpi_shutdown();
  return UACPI_INTERRUPT_HANDLED;
}

static uacpi_interrupt_ret handle_sleep_button(uacpi_handle ctx) {
  (void) ctx;
  return UACPI_INTERRUPT_HANDLED;
}

void acpi_power_init(void) {
  uacpi_status ret;

  printk(ACPI_BUTTON_CLASS "Installing Fixed Event Handlers...\n");

  // Clear any pending status

#ifdef ACPI_POWER_BUTTON
  uacpi_clear_fixed_event(UACPI_FIXED_EVENT_POWER_BUTTON);
  // Install Power Button Handler
  ret = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, handle_power_button, NULL);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR ACPI_BUTTON_CLASS "Failed to install Power Button handler: %s\n", uacpi_status_to_string(ret));
  } else {
    // Enable the event
    ret = uacpi_enable_fixed_event(UACPI_FIXED_EVENT_POWER_BUTTON);
    if (uacpi_unlikely_error(ret)) {
      printk(KERN_ERR ACPI_BUTTON_CLASS "Failed to enable Power Button event: %s\n", uacpi_status_to_string(ret));
    } else {
      printk(ACPI_BUTTON_CLASS "Power Button enabled.\n");
    }
  }
#endif

#ifdef ACPI_SLEEP_BUTTON
  uacpi_clear_fixed_event(UACPI_FIXED_EVENT_SLEEP_BUTTON);
  // Install Sleep Button Handler (Optional, just logging for now)
  ret = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_SLEEP_BUTTON, handle_sleep_button, NULL);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_WARNING ACPI_BUTTON_CLASS "Failed to install Sleep Button handler: %s\n", uacpi_status_to_string(ret));
  } else {
    // Enable the event
    ret = uacpi_enable_fixed_event(UACPI_FIXED_EVENT_SLEEP_BUTTON);
    if (uacpi_likely_success(ret)) {
      printk(ACPI_BUTTON_CLASS "Sleep Button enabled.\n");
    }
  }
#endif
}
