/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/fw/fw.c
 * @brief Firmware Interface Module Entry Point
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/fkx/fkx.h>

extern int smbios_init(void);
extern int nvram_init(void);
extern int efi_init(void);

static int fw_module_init(void) {
  smbios_init();
  nvram_init();
  efi_init();
  return 0;
}

FKX_MODULE_DEFINE(
  fw,
  "1.1.0",
  "assembler-0",
  "Firmware Subsystem (SMBIOS, NVRAM, UEFI)",
  FKX_FLAG_CORE,
  FKX_DRIVER_CLASS,
  KSYM_LICENSE_GPL,
  FKX_SUBCLASS_FW,
  FKX_NO_REQUIREMENTS,
  fw_module_init
);
