/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/fw/smbios.c
 * @brief SMBIOS Parsing Logic
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/fw.h>
#include <aerosync/sysintf/platform.h>
#include <aerosync/errno.h>
#include <arch/x86_64/requests.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <drivers/fw/smbios.h>

static struct firmware_device smbios_fw_dev;

static int smbios_get_entry(struct device *dev, uint8_t type, void **out_ptr, size_t *out_len) {
  (void)dev;
  volatile struct limine_smbios_request *req = get_smbios_request();

  if (!req || !req->response) return -ENODEV;

  void *table_addr = nullptr;
  uint32_t table_len = 0;

  if (req->response->entry_64) {
    struct smbios_entry_point_64 *ep64 = pmm_phys_to_virt(req->response->entry_64);
    table_addr = pmm_phys_to_virt(ep64->table_address);
    table_len = ep64->table_max_size;
  } else if (req->response->entry_32) {
    struct smbios_entry_point_32 *ep32 = pmm_phys_to_virt(req->response->entry_32);
    table_addr = pmm_phys_to_virt(ep32->table_address);
    table_len = ep32->table_length;
  } else {
    return -ENODEV;
  }

  uint8_t *ptr = (uint8_t *)table_addr;
  uint8_t *end = ptr + table_len;

  while (ptr < end) {
    struct smbios_header *hdr = (struct smbios_header *)ptr;
    if (hdr->type == 127) break; // End of table

    if (hdr->type == type) {
      *out_ptr = ptr;
      *out_len = hdr->length;
      return 0;
    }

    if (hdr->length < sizeof(struct smbios_header)) break;

    ptr += hdr->length;
    while (ptr < end - 1 && (ptr[0] != 0 || ptr[1] != 0)) {
      ptr++;
    }
    ptr += 2; 
  }

  return -ENOENT;
}

static const char* smbios_get_string(struct device *dev, void *entry, uint8_t index) {
  (void)dev;
  if (index == 0) return "N/A";

  struct smbios_header *hdr = (struct smbios_header *)entry;
  char *ptr = (char *)entry + hdr->length;

  for (uint8_t i = 1; i < index; i++) {
    while (*ptr != 0) ptr++;
    ptr++;
  }

  return ptr;
}

static int smbios_for_each(struct device *dev, int (*cb)(void *header, void *data), void *data) {
  (void)dev;
  volatile struct limine_smbios_request *req = get_smbios_request();

  if (!req || !req->response) return -ENODEV;

  void *table_addr = nullptr;
  uint32_t table_len = 0;

  if (req->response->entry_64) {
    struct smbios_entry_point_64 *ep64 = pmm_phys_to_virt(req->response->entry_64);
    table_addr = pmm_phys_to_virt(ep64->table_address);
    table_len = ep64->table_max_size;
  } else if (req->response->entry_32) {
    struct smbios_entry_point_32 *ep32 = pmm_phys_to_virt(req->response->entry_32);
    table_addr = pmm_phys_to_virt(ep32->table_address);
    table_len = ep32->table_length;
  } else {
    return -ENODEV;
  }

  uint8_t *ptr = (uint8_t *)table_addr;
  uint8_t *end = ptr + table_len;

  while (ptr < end) {
    struct smbios_header *hdr = (struct smbios_header *)ptr;
    if (hdr->type == 127) break;

    int ret = cb(hdr, data);
    if (ret != 0) return ret;

    if (hdr->length < sizeof(struct smbios_header)) break;

    ptr += hdr->length;
    while (ptr < end - 1 && (ptr[0] != 0 || ptr[1] != 0)) {
      ptr++;
    }
    ptr += 2;
  }
  return 0;
}

static struct smbios_ops s_smbios_ops = {
  .get_entry = smbios_get_entry,
  .get_string = smbios_get_string,
  .for_each_structure = smbios_for_each,
};

static int smbios_probe(struct platform_device *pdev) {
  struct firmware_device *fw_dev = container_of(pdev, struct firmware_device, pdev);
  fw_dev->ops = &s_smbios_ops;
  fw_dev->type = "smbios";
  
  printk(KERN_INFO SMBIOS_CLASS "SMBIOS driver initialized\n");

  // Smoke test
  void *entry;
  size_t len;
  if (smbios_get_entry(&pdev->dev, 0, &entry, &len) == 0) {
    const char *vendor = smbios_get_string(&pdev->dev, entry, 1);
    const char *version = smbios_get_string(&pdev->dev, entry, 2);
    printk(KERN_INFO SMBIOS_CLASS "BIOS Vendor: %s, Version: %s\n", vendor, version);
  }

  return 0;
}

static struct platform_driver smbios_driver = {
  .probe = smbios_probe,
  .driver = {
    .name = "smbios",
  },
};

int smbios_init(void) {
  platform_driver_register(&smbios_driver);

  smbios_fw_dev.pdev.name = "smbios";
  smbios_fw_dev.pdev.id = -1;
  smbios_fw_dev.pdev.dev.class = &fw_class;
  
  return platform_device_register(&smbios_fw_dev.pdev);
}

void smbios_deinit(void) {
  platform_device_unregister(&smbios_fw_dev.pdev);
  platform_driver_unregister(&smbios_driver);
}