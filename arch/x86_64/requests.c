/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/requests.c
 * @brief Limine requests
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

#include <arch/x86_64/requests.h>
#include <aerosync/fkx/fkx.h>

// Set Limine Request Start Marker
__attribute__((
  used,
  section(".limine_requests_start"))) static volatile
uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((
  used,
  section(".limine_requests"))) static volatile
uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(4);

#ifdef SIMPLE_FB_SUPPORT
__attribute__((
  used,
  section(".limine_requests"))) volatile
struct limine_framebuffer_request framebuffer_request = {
  .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0
};
#endif

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_memmap_request memmap_request = {
  .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_paging_mode_request paging_request = {
  .id = LIMINE_PAGING_MODE_REQUEST_ID,
  .revision = 0,
  .mode = LIMINE_PAGING_MODE_X86_64_5LVL
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_hhdm_request hhdm_request = {
  .id = LIMINE_HHDM_REQUEST_ID, .revision = 0
};

__attribute__((used,
  section(".limine_requests"))) volatile
struct limine_rsdp_request rsdp_request = {
  .id = LIMINE_RSDP_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_smbios_request smbios_request = {
  .id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_module_request module_request = {
  .id = LIMINE_MODULE_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_bootloader_info_request bootloader_info_request = {
  .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_bootloader_performance_request bootloader_performance_request = {
  .id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_executable_cmdline_request cmdline_request = {
  .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_executable_file_request executable_file_request = {
  .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID, .revision = 0
};

__attribute__((used, section(".limine_requests"))) static volatile
struct limine_firmware_type_request fw_request = {
  .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile
struct limine_date_at_boot_request date_at_boot_request = {
  .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
  .revision = 0
};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

volatile uint64_t *get_limine_base_revision(void) {
  return limine_base_revision;
}
EXPORT_SYMBOL(get_limine_base_revision);

volatile struct limine_memmap_request *get_memmap_request(void) {
  return &memmap_request;
}
EXPORT_SYMBOL(get_memmap_request);

volatile struct limine_framebuffer_request *get_framebuffer_request(void) {
#ifdef SIMPLE_FB_SUPPORT
  return &framebuffer_request;
#else
  return nullptr;
#endif
}
EXPORT_SYMBOL(get_framebuffer_request);

volatile struct limine_paging_mode_request *get_paging_request(void) {
  return &paging_request;
}
EXPORT_SYMBOL(get_paging_request);

volatile struct limine_hhdm_request *get_hhdm_request(void) {
  return &hhdm_request;
}
EXPORT_SYMBOL(get_hhdm_request);

volatile struct limine_smbios_request *get_smbios_request(void) {
  return &smbios_request;
}
EXPORT_SYMBOL(get_smbios_request);

volatile struct limine_module_request *get_module_request(void) {
  return &module_request;
}
EXPORT_SYMBOL(get_module_request);

volatile struct limine_bootloader_info_request *get_bootloader_info_request(void) {
  return &bootloader_info_request;
}
EXPORT_SYMBOL(get_bootloader_info_request);

volatile struct limine_bootloader_performance_request *get_bootloader_performance_request(void) {
  return &bootloader_performance_request;
}
EXPORT_SYMBOL(get_bootloader_performance_request);

volatile struct limine_executable_cmdline_request *get_cmdline_request(void) {
  return &cmdline_request;
}
EXPORT_SYMBOL(get_cmdline_request);

volatile struct limine_executable_file_request *get_executable_file_request(void) {
  return &executable_file_request;
}
EXPORT_SYMBOL(get_executable_file_request);

volatile struct limine_firmware_type_request *get_fw_request(void) {
  return &fw_request;
}
EXPORT_SYMBOL(get_fw_request);

volatile struct limine_date_at_boot_request *get_date_at_boot_request(void) {
  return &date_at_boot_request;
}
EXPORT_SYMBOL(get_date_at_boot_request);

volatile struct limine_rsdp_request *get_rsdp_request(void) {
  return &rsdp_request;
}
EXPORT_SYMBOL(get_rsdp_request);