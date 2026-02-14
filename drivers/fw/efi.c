/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/fw/efi.c
 * @brief UEFI Runtime Services Driver
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sysintf/fw.h>
#include <aerosync/sysintf/platform.h>
#include <aerosync/errno.h>
#include <arch/x86_64/requests.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

/* EFI Runtime Services Table */
struct efi_runtime_services {
  uint64_t hdr[3]; 
  uint64_t get_time;
  uint64_t set_time;
  uint64_t get_wakeup_time;
  uint64_t set_wakeup_time;
  uint64_t set_virtual_address_map;
  uint64_t convert_pointer;
  uint64_t get_variable;
  uint64_t get_next_variable;
  uint64_t set_variable;
  uint64_t get_next_high_mono_count;
  uint64_t reset_system;
  uint64_t update_capsule;
  uint64_t query_capsule_capabilities;
  uint64_t query_variable_info;
} __packed;

struct efi_system_table {
  uint64_t hdr[3];
  uint64_t firmware_vendor;
  uint32_t firmware_revision;
  uint32_t padding;
  uint64_t console_in_handle;
  uint64_t con_in;
  uint64_t console_out_handle;
  uint64_t con_out;
  uint64_t standard_error_handle;
  uint64_t std_err;
  uint64_t runtime_services;
} __packed;

static struct efi_runtime_services *s_runtime = nullptr;
static struct firmware_device efi_fw_dev;

typedef uint64_t (__ms_abi *efi_get_variable_t)(const char16_t *name, const efi_guid_t *vendor, 
                                               uint32_t *attr, uint64_t *data_size, void *data);

typedef uint64_t (__ms_abi *efi_set_variable_t)(const char16_t *name, const efi_guid_t *vendor, 
                                               uint32_t attr, uint64_t data_size, void *data);

typedef uint64_t (__ms_abi *efi_get_next_variable_t)(uint64_t *name_size, char16_t *name, efi_guid_t *vendor);

typedef void (__ms_abi *efi_reset_system_t)(int type, uint64_t status, uint64_t data_size, void *data);

static int efi_get_var(const char16_t *name, const efi_guid_t *vendor, 
                       uint32_t *attr, size_t *data_size, void *data) {
  if (!s_runtime || !s_runtime->get_variable) return -ENODEV;

  efi_get_variable_t func = (efi_get_variable_t)s_runtime->get_variable;
  uint64_t sz = *data_size;
  uint64_t status = func(name, vendor, attr, &sz, data);
  *data_size = (size_t)sz;

  if (status == 0) return 0;
  if (status == 0x8000000000000005) return -ERANGE; 
  return -EIO;
}

static int efi_set_var(const char16_t *name, const efi_guid_t *vendor, 
                       uint32_t attr, size_t data_size, void *data) {
  if (!s_runtime || !s_runtime->set_variable) return -ENODEV;

  efi_set_variable_t func = (efi_set_variable_t)s_runtime->set_variable;
  uint64_t status = func(name, vendor, attr, (uint64_t)data_size, data);

  return (status == 0) ? 0 : -EIO;
}

static int efi_get_next_var(size_t *name_size, char16_t *name, efi_guid_t *vendor) {
  if (!s_runtime || !s_runtime->get_next_variable) return -ENODEV;

  efi_get_next_variable_t func = (efi_get_next_variable_t)s_runtime->get_next_variable;
  uint64_t sz = *name_size;
  uint64_t status = func(&sz, name, vendor);
  *name_size = (size_t)sz;

  if (status == 0) return 0;
  if (status == 0x8000000000000005) return -ERANGE;
  return -ENOENT;
}

static int efi_reset(int type) {
  if (!s_runtime || !s_runtime->reset_system) return -ENODEV;
  efi_reset_system_t func = (efi_reset_system_t)s_runtime->reset_system;
  func(type, 0, 0, nullptr);
  return 0; // Should not return
}

static struct efi_ops s_efi_ops = {
  .get_variable = efi_get_var,
  .set_variable = efi_set_var,
  .get_next_variable = efi_get_next_var,
  .reset_system = efi_reset,
};

static int efi_probe(struct platform_device *pdev) {
  struct firmware_device *fw_dev = container_of(pdev, struct firmware_device, pdev);
  fw_dev->ops = &s_efi_ops;
  fw_dev->type = "efi";

  printk(KERN_INFO FW_CLASS "UEFI Runtime Services driver initialized\n");
  return 0;
}

static struct platform_driver efi_driver = {
  .probe = efi_probe,
  .driver = { .name = "efi" },
};

int efi_init(void) {
  volatile struct limine_efi_system_table_request *req = get_efi_system_table_request();

  if (!req || !req->response) return -ENODEV;

  struct efi_system_table *st = pmm_phys_to_virt(req->response->address);
  s_runtime = pmm_phys_to_virt(st->runtime_services);

  platform_driver_register(&efi_driver);
  efi_fw_dev.pdev.name = "efi";
  efi_fw_dev.pdev.id = -1;
  efi_fw_dev.pdev.dev.class = &fw_class;
  return platform_device_register(&efi_fw_dev.pdev);
}
