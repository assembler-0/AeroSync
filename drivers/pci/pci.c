#include <drivers/pci/pci.h>
#include <arch/x64/io.h>

// Helper for PCI config read/write
uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  if (p->segment != 0) return 0xFFFFFFFF;

  uint32_t address =
      (1u << 31) |
      ((uint32_t)p->bus     << 16) |
      ((uint32_t)p->device  << 11) |
      ((uint32_t)p->function<< 8)  |
      (offset & 0xFC);

  outl(0xCF8, address);
  uint32_t val = inl(0xCFC);

  uint32_t shift = (offset & 3) * 8;

  if (width == 8) return (val >> shift) & 0xFF;
  if (width == 16) return (val >> shift) & 0xFFFF;
  return val;
}

void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
  if (p->segment != 0) return;

  uint32_t address =
      (UINT32_C(1) << 31) |
      ((uint32_t)p->bus     << 16) |
      ((uint32_t)p->device  << 11) |
      ((uint32_t)p->function<< 8)  |
      ((uint32_t)offset & 0xFC);


  outl(0xCF8, address);

  if (width != 32) {
    uint32_t current_val = inl(0xCFC);
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = ((1ULL << width) - 1) << shift;
    current_val &= ~mask;
    current_val |= (val << shift);
    outl(0xCFC, current_val);
  } else {
    outl(0xCFC, val);
  }
}