/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/fw.c
 * @brief Generic Firmware Interface Subsystem Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/fw.h>
#include <aerosync/classes.h>
#include <aerosync/export.h>
#include <arch/x86_64/cpu.h>
#include <lib/printk.h>
#include <lib/string.h>

struct class fw_class = {
  .name = "firmware",
  .dev_prefix = "fw",
  .naming_scheme = NAMING_NUMERIC,
};

static int __init __no_cfi dump_mem_device_cb(void *header, void *data) {
  struct smbios_header *hdr = header;
  struct { struct device *dev; struct smbios_ops *ops; } *info = data;

  if (hdr->type != 17) return 0;

  struct smbios_type17 *mem = header;
  if (mem->size == 0 || mem->size == 0xFFFF) return 0;

  const char *locator = info->ops->get_string(info->dev, header, mem->device_locator);
  const char *bank = info->ops->get_string(info->dev, header, mem->bank_locator);
  const char *type = "Unknown";

  switch (mem->memory_type) {
    case 0x01: type = "Other"; break;
    case 0x02: type = "Unknown"; break;
    case 0x03: type = "DRAM"; break;
    case 0x0F: type = "SDRAM"; break;
    case 0x12: type = "DDR"; break;
    case 0x13: type = "DDR2"; break;
    case 0x18: type = "DDR3"; break;
    case 0x1A: type = "DDR4"; break;
    case 0x22: type = "DDR5"; break;
    default: break;
  }

  uint32_t size_mb = mem->size;
  if (size_mb & 0x8000) {
    size_mb = (size_mb & 0x7FFF) / 1024; // KB to MB
  }

  printk(KERN_DEBUG FW_CLASS "|- Memory  : %u MB %s (%s / %s)\n", 
         size_mb, type, locator, bank);
  return 0;
}

void __no_cfi fw_dump_hardware_info(void) {
  struct device *dev = device_find_by_name("smbios");
  if (!dev) return;

  struct firmware_device *fw = (struct firmware_device *)dev;
  struct smbios_ops *ops = (struct smbios_ops *)fw->ops;
  if (!ops) {
    put_device(dev);
    return;
  }

  void *entry;
  size_t len;

  printk(KERN_DEBUG FW_CLASS "[--- hwinfo (smbios) ---]\n");

  // 0. BIOS Information (Type 0)
  if (ops->get_entry(dev, 0, &entry, &len) == 0) {
    struct smbios_type0 *bios = entry;
    printk(KERN_DEBUG FW_CLASS "|- BIOS    : %s %s (%s)\n",
           ops->get_string(dev, entry, bios->vendor),
           ops->get_string(dev, entry, bios->version),
           ops->get_string(dev, entry, bios->release_date));
  }

  // 1. System Information (Type 1)
  if (ops->get_entry(dev, 1, &entry, &len) == 0) {
    uint8_t *data = entry;
    printk(KERN_DEBUG FW_CLASS "|- System  : %s %s\n",
           ops->get_string(dev, entry, data[0x04]),
           ops->get_string(dev, entry, data[0x05]));
  }

  // 2. Baseboard Information (Type 2)
  if (ops->get_entry(dev, 2, &entry, &len) == 0) {
    uint8_t *data = entry;
    printk(KERN_DEBUG FW_CLASS "|- Board   : %s %s\n",
           ops->get_string(dev, entry, data[0x04]),
           ops->get_string(dev, entry, data[0x05]));
  }

  // 3. Chassis Information (Type 3)
  if (ops->get_entry(dev, 3, &entry, &len) == 0) {
    uint8_t *data = entry;
    printk(KERN_DEBUG FW_CLASS "|- Chassis : %s %s\n",
           ops->get_string(dev, entry, data[0x04]),
           ops->get_string(dev, entry, data[0x06]));
  }

  // 4. Processor Information (Type 4)
  if (ops->get_entry(dev, 4, &entry, &len) == 0) {
    uint8_t *data = entry;
    char vendor[13];
    cpuid_get_vendor(vendor);

    printk(KERN_DEBUG FW_CLASS "|- CPU     : %s %s [%s]%s\n",
           ops->get_string(dev, entry, data[0x07]),
           ops->get_string(dev, entry, data[0x10]),
           vendor,
           is_host_hypervisor() ? " (Hypervisor)" : "");
  }

  // 5. Memory Devices (Type 17)
  struct { struct device *dev; struct smbios_ops *ops; } info = { dev, ops };
  ops->for_each_structure(dev, dump_mem_device_cb, &info);

  put_device(dev);
}

int fw_init(void) {
  int ret = class_register(&fw_class);
  if (ret) {
    printk(KERN_ERR FW_CLASS "failed to register firmware class: %d\n", ret);
    return -EFAULT;
  }
  printk(KERN_INFO FW_CLASS "firmware subsystem initialized\n");
  return 0;
}

int firmware_device_register(struct firmware_device *fw_dev) {
  fw_dev->pdev.dev.class = &fw_class;
  return device_register(&fw_dev->pdev.dev);
}

void firmware_device_unregister(struct firmware_device *fw_dev) {
  device_unregister(&fw_dev->pdev.dev);
}

EXPORT_SYMBOL(fw_class);
EXPORT_SYMBOL(fw_init);
EXPORT_SYMBOL(fw_dump_hardware_info);
EXPORT_SYMBOL(firmware_device_register);
EXPORT_SYMBOL(firmware_device_unregister);
