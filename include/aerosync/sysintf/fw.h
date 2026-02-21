/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/fw.h
 * @brief Generic Firmware Interface Subsystem (Linux-grade)
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/platform.h>
#include <aerosync/errno.h>
#include <aerosync/compiler.h>

/**
 * struct firmware_device - Generic firmware device structure
 */
struct firmware_device {
  struct platform_device pdev;
  const char *type;
  void *ops;
};

/**
 * SMBIOS Structures
 */
struct smbios_header {
  uint8_t type;
  uint8_t length;
  uint16_t handle;
} __packed;

struct smbios_type0 {
  struct smbios_header hdr;
  uint8_t vendor;
  uint8_t version;
  uint16_t start_address;
  uint8_t release_date;
  uint8_t rom_size;
  uint64_t characteristics;
  uint8_t extension[2];
} __packed;

struct smbios_type17 {
  struct smbios_header hdr;
  uint16_t array_handle;
  uint16_t error_handle;
  uint16_t total_width;
  uint16_t data_width;
  uint16_t size;
  uint8_t form_factor;
  uint8_t device_set;
  uint8_t device_locator;
  uint8_t bank_locator;
  uint8_t memory_type;
  uint16_t type_detail;
  uint16_t speed;
  uint8_t manufacturer;
  uint8_t serial_number;
  uint8_t asset_tag;
  uint8_t part_number;
} __packed;

/**
 * SMBIOS Operations
 */
struct smbios_ops {
  int (*get_entry)(struct device *dev, uint8_t type, void **out_ptr, size_t *out_len);
  const char* (*get_string)(struct device *dev, void *entry, uint8_t index);
  int (*for_each_structure)(struct device *dev, int (*cb)(void *header, void *data), void *data);
};

/**
 * NVRAM / CMOS Operations (Legacy)
 */
struct nvram_ops {
  uint8_t (*read)(struct device *dev, uint16_t offset);
  void (*write)(struct device *dev, uint16_t offset, uint8_t val);
  size_t (*get_size)(struct device *dev);
};

/**
 * UEFI Variable Attributes
 */
#define EFI_VARIABLE_NON_VOLATILE                          0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                 0x00000008
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS            0x00000010
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020
#define EFI_VARIABLE_APPEND_WRITE                          0x00000040

typedef struct {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t  data4[8];
} __packed efi_guid_t;

/**
 * UEFI Runtime Services Operations
 */
struct efi_ops {
  int (*get_variable)(const char16_t *name, const efi_guid_t *vendor, 
                      uint32_t *attr, size_t *data_size, void *data);
  int (*set_variable)(const char16_t *name, const efi_guid_t *vendor, 
                      uint32_t attr, size_t data_size, void *data);
  int (*get_next_variable)(size_t *name_size, char16_t *name, efi_guid_t *vendor);
  
  int (*reset_system)(int type); // 0=Cold, 1=Warm, 2=Shutdown
};

/**
 * fw_class - The global firmware class
 */
extern struct class fw_class;

/**
 * fw_init - Initialize firmware subsystem
 */
int __must_check fw_init(void);

/**
 * fw_dump_hardware_info - Dump hardware information to log
 */
void fw_dump_hardware_info(void);

/**
 * firmware_device_register - Register a firmware device
 */
int firmware_device_register(struct firmware_device *fw_dev);

/**
 * firmware_device_unregister - Unregister a firmware device
 */
void firmware_device_unregister(struct firmware_device *fw_dev);
