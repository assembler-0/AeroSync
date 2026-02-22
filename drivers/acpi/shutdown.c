/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/shutdown.c
 * @brief ACPI Shutdown and Reboot Implementation using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <drivers/acpi/power.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/sysintf/udm.h>

void acpi_shutdown(void) {
  printk(POWER_CLASS "Preparing for S5 Soft Off...\n");

  ACPI_STATUS ret = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
  if (ACPI_FAILURE(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to prepare for S5: %s\n", AcpiFormatException(ret));
    return;
  }

#ifdef ACPI_POWER_KERNEL_DEINITIALIZE
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  const int k_ret = udm_shutdown_all();
  if (k_ret != 0) {
    printk(KERN_ERR POWER_CLASS "Failed to shutdown devices: %d\n", k_ret);
    return;
  }

  printk_disable();
#endif

  ret = AcpiEnterSleepState(ACPI_STATE_S5);
  if (ACPI_FAILURE(ret)) {
    printk(KERN_ERR POWER_CLASS "Failed to enter S5: %s\n", AcpiFormatException(ret));
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
    printk(KERN_ERR POWER_CLASS "Failed to shutdown devices: %d\n", k_ret);
    return;
  }

  printk_disable();
#endif

  ACPI_STATUS ret = AcpiReset();
  if (ACPI_FAILURE(ret)) {
    printk(KERN_ERR POWER_CLASS "ACPI Reboot failed: %s\n", AcpiFormatException(ret));
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
