/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/fw/fw.c
 * @brief Firmware Interface Module Entry Point
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/asrx.h>

extern int smbios_init(void);
extern int nvram_init(void);
extern int efi_init(void);

static int fw_module_init(void) {
  smbios_init();
  nvram_init();
  efi_init();
  return 0;
}

static void fw_module_exit(void) { }

asrx_module_init(fw_module_init);
asrx_module_exit(fw_module_exit);
asrx_module_info(fw, KSYM_LICENSE_GPL);