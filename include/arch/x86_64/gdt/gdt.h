#pragma once

#include <aerosync/types.h>
#include <compiler.h>

#define GDT_ACCESS_CODE_PL0 0x9A // Present, Ring 0, Executable, Read/Write
#define GDT_ACCESS_DATA_PL0 0x92 // Present, Ring 0, Read/Write
#define GDT_ACCESS_CODE_PL3 0xFA // Present, Ring 3, Executable, Read/Write
#define GDT_ACCESS_DATA_PL3 0xF2 // Present, Ring 3, Read/Write
#define GDT_ACCESS_TSS 0x89      // Present, Ring 0, TSS

#define GDT_FLAG_64BIT 0x20   // 64-bit code segment (L-bit)
#define GDT_FLAG_DB 0x40      // Default operation size (D/B)
#define GDT_FLAG_4K_GRAN 0x80 // 4KB granularity (G-bit)

// In long mode:
// - Code: L=1 (64-bit), D=0; G=1 (4KB granularity) for compatibility
// - Data: L=0; D/B=0 for 64-bit mode compatibility; G=1 for 4KB granularity
#define GDT_GRAN_CODE (GDT_FLAG_64BIT | GDT_FLAG_4K_GRAN)
#define GDT_GRAN_DATA (GDT_FLAG_4K_GRAN)

// GDT segment selectors
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define USER_CODE_SELECTOR 0x20
#define USER_DATA_SELECTOR 0x18
#define TSS_SELECTOR 0x28 // New TSS selector

// GDT entry structure
struct gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __packed;

// GDT pointer structure
struct gdt_ptr {
  uint16_t limit;
  uint64_t base;
} __packed;

// TSS structure (simplified for 64-bit)
struct tss_entry {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist1;
  uint64_t ist2;
  uint64_t ist3;
  uint64_t ist4;
  uint64_t ist5;
  uint64_t ist6;
  uint64_t ist7;
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap_base;
} __packed;

void gdt_init(void);
void gdt_init_ap(void);
void set_tss_rsp0(uint64_t rsp0);