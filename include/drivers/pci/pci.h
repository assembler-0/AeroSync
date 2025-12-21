#pragma once

#include <kernel/types.h>

typedef struct {
  uint16_t segment;
  uint8_t bus;
  uint8_t device;
  uint8_t function;
} pci_handle_t;

uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width);
void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width);