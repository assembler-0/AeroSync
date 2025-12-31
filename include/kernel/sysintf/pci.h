#pragma once

#include <kernel/types.h>
#include <drivers/pci/pci.h>

typedef struct {
  const char *name;
  uint32_t (*read)(pci_handle_t *p, uint32_t offset, uint8_t width);
  void (*write)(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width);
  int (*probe)(void);
  int priority;
} pci_ops_t;

void pci_register_ops(const pci_ops_t *ops);
uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width);
void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width);
