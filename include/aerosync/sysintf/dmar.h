/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/dmar.h
 * @brief Generic DMAR (DMA Remapping Reporting Table) Parser Interface
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <uacpi/acpi.h>
#include <linux/list.h>
#include <aerosync/compiler.h>

/* DMAR Subtable Types */
enum dmar_type {
  DMAR_TYPE_DRHD = 0,
  DMAR_TYPE_RMRR = 1,
  DMAR_TYPE_ATSR = 2,
  DMAR_TYPE_RHSA = 3,
  DMAR_TYPE_ANDD = 4,
  DMAR_TYPE_SATC = 5,
};

/* Device Scope Structure */
typedef struct {
  uint8_t type;
  uint8_t length;
  uint8_t enumeration_id;
  uint8_t start_bus;
  struct {
    uint8_t device;
    uint8_t function;
  } __packed path[];
} __packed dmar_device_scope_t;

/* DRHD Structure */
typedef struct {
  struct list_head node;
  uint16_t segment;
  uint64_t address;
  uint8_t flags;
  struct list_head devices; /* List of dmar_dev_t */
} dmar_unit_t;

/* RMRR Structure */
typedef struct {
  struct list_head node;
  uint16_t segment;
  uint64_t base_address;
  uint64_t end_address;
  struct list_head devices; /* List of dmar_dev_t */
} dmar_reserved_region_t;

/* ATSR Structure */
typedef struct {
  struct list_head node;
  uint16_t segment;
  uint8_t flags;
  struct list_head devices; /* List of dmar_dev_t */
} dmar_atsr_t;

/* Device representation in DMAR scope */
typedef struct {
  struct list_head node;
  uint8_t bus;
  uint8_t devfn;
} dmar_dev_t;

/**
 * @brief Initializes the DMAR manager by parsing the ACPI DMAR table.
 */
int dmar_init(void);

/**
 * @brief Accessors for parsed DMAR data
 */
struct list_head *dmar_get_units(void);
struct list_head *dmar_get_reserved_regions(void);
struct list_head *dmar_get_atsr_units(void);
