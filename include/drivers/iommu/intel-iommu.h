/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/drivers/iommu/intel-iommu.h
 * @brief Intel VT-d IOMMU definitions
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <compiler.h>
#include <linux/list.h>

/* VT-d Register Offsets */
#define DMAR_VER_REG            0x00    /* Version Register */
#define DMAR_CAP_REG            0x08    /* Capability Register */
#define DMAR_ECAP_REG           0x10    /* Extended Capability Register */
#define DMAR_GCMD_REG           0x18    /* Global Command Register */
#define DMAR_GSTS_REG           0x1c    /* Global Status Register */
#define DMAR_RTADDR_REG         0x20    /* Root-entry Table Address Register */
#define DMAR_CCMD_REG           0x28    /* Context Command Register */
#define DMAR_IQH_REG            0x80    /* Invalidation Queue Head Register */
#define DMAR_IQT_REG            0x88    /* Invalidation Queue Tail Register */
#define DMAR_IQA_REG            0x90    /* Invalidation Queue Address Register */
#define DMAR_ICS_REG            0x9c    /* Invalidation Completion Status Register */
#define DMAR_IRTA_REG           0xb8    /* Interrupt Remapping Table Address Register */
#define DMAR_IOTLB_REG          0x08    /* IOTLB Invalidation Register (relative to ECAP_REG_IRO) */

#define DMAR_GCMD_TE            (1U << 31) /* Translation Enable */
#define DMAR_GCMD_SRTP          (1U << 30) /* Set Root Table Pointer */
#define DMAR_GSTS_TES           (1U << 31) /* Translation Enable Status */
#define DMAR_GSTS_RTPS          (1U << 30) /* Root Table Pointer Status */

/* CAP_REG fields */
#define CAP_ND(c)               ((c) & 0x7ULL)
#define CAP_SAGAW(c)            (((c) >> 8) & 0x1fULL)
#define CAP_FRO(c)              (((c) >> 24) & 0x3ffULL)
#define CAP_NFR(c)              (((c) >> 40) & 0xffULL)

/* ECAP_REG fields */
#define ECAP_IRO(e)             (((e) >> 8) & 0x3ffULL)

/* Root Entry */
struct root_entry {
  uint64_t lo;
  uint64_t hi;
} __packed;

#define ROOT_PRESENT            (1ULL << 0)

/* Context Entry */
struct context_entry {
  uint64_t lo;
  uint64_t hi;
} __packed;

#define CONTEXT_PRESENT         (1ULL << 0)
#define CONTEXT_FPD             (1ULL << 1)
#define CONTEXT_TT_MULTI_LEVEL  (0ULL << 2)
#define CONTEXT_TT_PASSTHROUGH  (2ULL << 2)
#define CONTEXT_DID(d)          ((uint64_t)(d) << 8)
#define CONTEXT_ADDR_MASK       (~0xfffULL)
#define CONTEXT_AW_3LEVEL       (0ULL << 0)
#define CONTEXT_AW_4LEVEL       (1ULL << 0)

/* Page Table Entry */
#define VTD_PTE_R               (1ULL << 0)
#define VTD_PTE_W               (1ULL << 1)
#define VTD_PTE_ADDR_MASK       (((1ULL << 52) - 1) & ~0xfffULL)

struct intel_iommu {
  uint64_t reg_phys;
  void *reg_virt;
  uint32_t segment;
  uint64_t cap;
  uint64_t ecap;
  uint32_t gcmd;

  spinlock_t lock;
  struct root_entry *root_entry;
  struct list_head node;
};

struct dmar_domain {
  int id;
  uint64_t *pgtbl;
  uint64_t pgtbl_phys;
  int addr_width; /* 3 or 4 level */
  spinlock_t lock;
};

struct intel_iommu *find_iommu_for_device(uint16_t segment, uint8_t bus, uint8_t devfn);
