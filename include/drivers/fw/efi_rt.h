/// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <aerosync/types.h>
#include <aerosync/compiler.h>

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