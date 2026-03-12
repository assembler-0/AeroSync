/// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <aerosync/types.h>
#include <aerosync/compiler.h>

struct smbios_entry_point_32 {
  char anchor[4];
  uint8_t checksum;
  uint8_t length;
  uint8_t major;
  uint8_t minor;
  uint16_t max_structure_size;
  uint8_t revision;
  uint8_t formatted[5];
  char intermediate_anchor[5];
  uint8_t intermediate_checksum;
  uint16_t table_length;
  uint32_t table_address;
  uint16_t entry_count;
  uint8_t bcd_revision;
} __packed;

struct smbios_entry_point_64 {
  char anchor[5];
  uint8_t checksum;
  uint8_t length;
  uint8_t major;
  uint8_t minor;
  uint8_t doc_rev;
  uint8_t revision;
  uint8_t reserved;
  uint32_t table_max_size;
  uint64_t table_address;
} __packed;