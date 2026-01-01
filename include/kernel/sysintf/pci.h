/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file include/kernel/sysintf/pci.h
 * @brief System Interface for PCI Subsystem
 * @copyright (C) 2025 assembler-0
 */

#pragma once

#include <kernel/types.h>
#include <linux/list.h>

/* PCI Configuration space offsets */
#define PCI_VENDOR_ID           0x00
#define PCI_DEVICE_ID           0x02
#define PCI_COMMAND             0x04
#define PCI_STATUS              0x06
#define PCI_REVISION_ID         0x08
#define PCI_PROG_IF             0x09
#define PCI_SUBCLASS            0x0a
#define PCI_CLASS_CODE          0x0b
#define PCI_CACHE_LINE_SIZE     0x0c
#define PCI_LATENCY_TIMER       0x0d
#define PCI_HEADER_TYPE         0x0e
#define PCI_BIST                0x0f
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14
#define PCI_BAR2                0x18
#define PCI_BAR3                0x1c
#define PCI_BAR4                0x20
#define PCI_BAR5                0x24

#define PCI_COMMAND_IO		0x1
#define PCI_COMMAND_MEMORY	0x2
#define PCI_COMMAND_MASTER	0x4
#define PCI_COMMAND_SPECIAL	0x8
#define PCI_COMMAND_INVALIDATE	0x10
#define PCI_COMMAND_VGA_PALETTE	0x20
#define PCI_COMMAND_PARITY	0x40
#define PCI_COMMAND_WAIT	0x80
#define PCI_COMMAND_SERR	0x100
#define PCI_COMMAND_FAST_BACK	0x200
#define PCI_COMMAND_INTX_DISABLE 0x400

#define PCI_ANY_ID (~0U)

typedef struct {
  uint16_t segment;
  uint8_t bus;
  uint8_t device;
  uint8_t function;
} pci_handle_t;

struct pci_dev;
struct pci_bus;

struct pci_device_id {
    uint32_t vendor, device;
    uint32_t subvendor, subdevice;
    uint32_t class, class_mask;
    unsigned long driver_data;
};

struct pci_driver {
    struct list_head node;
    const char *name;
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
};

struct pci_bus {
    struct list_head node;
    struct list_head devices;
    struct list_head children;
    struct pci_bus *parent;
    uint16_t segment;
    uint8_t number;
};

struct pci_dev {
    struct list_head bus_list;
    struct list_head global_list;
    struct pci_bus *bus;
    uint16_t devfn;
    uint16_t vendor;
    uint16_t device;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint32_t class;
    uint8_t revision;
    uint8_t hdr_type;

    struct pci_driver *driver;
    pci_handle_t handle;

    uint32_t bars[6];
    uint32_t bar_sizes[6];
};

/* Hardware Access Ops */
typedef struct {
  const char *name;
  uint32_t (*read)(pci_handle_t *p, uint32_t offset, uint8_t width);
  void (*write)(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width);
  int (*probe)(void);
  int priority;
} pci_ops_t;

/* High-level Subsystem Ops */
typedef struct {
    int (*register_driver)(struct pci_driver *driver);
    void (*unregister_driver)(struct pci_driver *driver);
    void (*enumerate_bus)(struct pci_bus *bus);
    int (*enable_device)(struct pci_dev *dev);
    void (*set_master)(struct pci_dev *dev);
} pci_subsystem_ops_t;

/* Registration API */
void pci_register_ops(const pci_ops_t *ops);
void pci_register_subsystem(const pci_subsystem_ops_t *ops);

/* Low-level Config Access */
uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width);
void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width);

/* High-level PCI API */
int pci_register_driver(struct pci_driver *driver);
void pci_unregister_driver(struct pci_driver *driver);
void pci_enumerate_bus(struct pci_bus *bus);
int pci_enable_device(struct pci_dev *dev);
void pci_set_master(struct pci_dev *dev);

/* Helpers */
static inline pci_handle_t pci_dev_to_handle(struct pci_dev *dev) {
    return dev->handle;
}

static inline uint32_t pci_read_config8(struct pci_dev *dev, int where) {
    return pci_read(&dev->handle, where, 8);
}
static inline uint32_t pci_read_config16(struct pci_dev *dev, int where) {
    return pci_read(&dev->handle, where, 16);
}
static inline uint32_t pci_read_config32(struct pci_dev *dev, int where) {
    return pci_read(&dev->handle, where, 32);
}

static inline void pci_write_config8(struct pci_dev *dev, int where, uint8_t val) {
    pci_write(&dev->handle, where, val, 8);
}
static inline void pci_write_config16(struct pci_dev *dev, int where, uint16_t val) {
    pci_write(&dev->handle, where, val, 16);
}
static inline void pci_write_config32(struct pci_dev *dev, int where, uint32_t val) {
    pci_write(&dev->handle, where, val, 32);
}

#define PCI_DEVFN(slot, func)	((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)		(((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)		((devfn) & 0x07)