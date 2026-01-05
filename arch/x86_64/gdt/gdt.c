/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/gdt/gdt.c
 * @brief Global Descriptor Table (GDT) setup for x86-64
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt/gdt.h>
#include <arch/x86_64/percpu.h>
#include <compiler.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// GDT with 7 entries: null, kcode, kdata, ucode, udata, tss_low, tss_high
// CRITICAL: GDT must be properly aligned for x86-64
// CRITICAL: GDT must be properly aligned for x86-64
DEFINE_PER_CPU(struct gdt_entry, gdt_entries[7]);
DEFINE_PER_CPU(struct tss_entry, tss_entry);

extern void gdt_flush(const struct gdt_ptr *gdt_ptr_addr);
extern void tss_flush(void);

static volatile int gdt_lock = 0;

static void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  struct gdt_entry *gdt = (struct gdt_entry *)this_cpu_ptr(gdt_entries);
  irq_flags_t flags = spinlock_lock_irqsave(&gdt_lock);
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;

  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = (limit >> 16) & 0x0F;
  gdt[num].granularity |= gran & 0xF0;
  gdt[num].access = access;
  spinlock_unlock_irqrestore(&gdt_lock, flags);
}

// A cleaner way to set up the 64-bit TSS descriptor
static void set_tss_gate(int num, uint64_t base, uint64_t limit) {
  struct gdt_entry *gdt = (struct gdt_entry *)this_cpu_ptr(gdt_entries);
  irq_flags_t flags = spinlock_lock_irqsave(&gdt_lock);
  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].access = GDT_ACCESS_TSS; // Access byte for 64-bit TSS
  gdt[num].granularity =
      (limit >> 16) & 0x0F; // No granularity bits (G=0, AVL=0)
  gdt[num].base_high = (base >> 24) & 0xFF;

  uint32_t *base_high_ptr = (uint32_t *)&gdt[num + 1];
  *base_high_ptr = (base >> 32);

  *((uint32_t *)base_high_ptr + 1) = 0;
  spinlock_unlock_irqrestore(&gdt_lock, flags);
}

void gdt_init(void) {
  struct gdt_ptr gdt_ptr;
  gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
  gdt_ptr.base = (uint64_t)(struct gdt_entry *)this_cpu_ptr(gdt_entries);

  set_gdt_gate(0, 0, 0, 0, 0); // 0x00: Null segment
  set_gdt_gate(1, 0, 0xFFFFFFFF, GDT_ACCESS_CODE_PL0,
               GDT_GRAN_CODE); // 0x08: Kernel Code
  set_gdt_gate(2, 0, 0xFFFFFFFF, GDT_ACCESS_DATA_PL0,
               GDT_GRAN_DATA); // 0x10: Kernel Data
  set_gdt_gate(3, 0, 0xFFFFFFFF, GDT_ACCESS_DATA_PL3,
               GDT_GRAN_DATA); // 0x18: User Data
  set_gdt_gate(4, 0, 0xFFFFFFFF, GDT_ACCESS_CODE_PL3,
               GDT_GRAN_CODE); // 0x20: User Code

  struct tss_entry *tss = this_cpu_ptr(tss_entry);
  uint64_t tss_base = (uint64_t)tss;
  uint64_t tss_limit = sizeof(struct tss_entry) - 1;
  set_tss_gate(5, tss_base, tss_limit);

  tss->iomap_base = sizeof(struct tss_entry);

  gdt_flush(&gdt_ptr);
  tss_flush();
}

void gdt_init_ap(void) {
  gdt_init();
}

void set_tss_rsp0(uint64_t rsp0) { this_cpu_write(tss_entry.rsp0, rsp0); }