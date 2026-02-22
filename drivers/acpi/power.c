/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/power.c
 * @brief ACPI Power Button and Sleep Button handling using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <drivers/acpi/power.h>
#include <lib/printk.h>

static uint32_t handle_power_button(void *ctx) {
  (void)ctx;
  acpi_shutdown();
  return ACPI_INTERRUPT_HANDLED;
}

static uint32_t handle_sleep_button(void *ctx) {
  (void)ctx;
  return ACPI_INTERRUPT_HANDLED;
}

int acpi_power_init(void) {
  ACPI_STATUS ret;
  int status = 0;

  printk(ACPI_BUTTON_CLASS "Installing Fixed Event Handlers...\n");

#ifdef ACPI_POWER_BUTTON
  AcpiClearEvent(ACPI_EVENT_POWER_BUTTON);
  // Install Power Button Handler
  ret = AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON,
                                     handle_power_button, nullptr);
  if (ACPI_FAILURE(ret)) {
    printk(KERN_ERR ACPI_BUTTON_CLASS
           "Failed to install Power Button handler: %s\n",
           AcpiFormatException(ret));
    status = -EIO;
  } else {
    // Enable the event
    ret = AcpiEnableEvent(ACPI_EVENT_POWER_BUTTON, 0);
    if (ACPI_FAILURE(ret)) {
      printk(KERN_ERR ACPI_BUTTON_CLASS
             "Failed to enable Power Button event: %s\n",
             AcpiFormatException(ret));
      status = -EIO;
    } else {
      printk(ACPI_BUTTON_CLASS "Power Button enabled.\n");
    }
  }
#endif

#ifdef ACPI_SLEEP_BUTTON
  AcpiClearEvent(ACPI_EVENT_SLEEP_BUTTON);
  // Install Sleep Button Handler
  ret = AcpiInstallFixedEventHandler(ACPI_EVENT_SLEEP_BUTTON,
                                     handle_sleep_button, nullptr);
  if (ACPI_FAILURE(ret)) {
    printk(KERN_WARNING ACPI_BUTTON_CLASS
           "Failed to install Sleep Button handler: %s\n",
           AcpiFormatException(ret));
    status = -EIO;
  } else {
    // Enable the event
    ret = AcpiEnableEvent(ACPI_EVENT_SLEEP_BUTTON, 0);
    if (ACPI_SUCCESS(ret)) {
      printk(ACPI_BUTTON_CLASS "Sleep Button enabled.\n");
    } else {
      printk(KERN_WARNING ACPI_BUTTON_CLASS
             "Failed to enable Sleep Button event: %s\n",
             AcpiFormatException(ret));
      status = -EIO;
    }
  }
#endif
  return status;
}
