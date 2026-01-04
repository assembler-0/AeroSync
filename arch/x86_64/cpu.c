/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/cpu.c
 * @brief Architecture-specific CPU functions
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
#include <arch/x86_64/percpu.h>
#include <kernel/fkx/fkx.h>

DEFINE_PER_CPU(unsigned long, this_cpu_off);
DEFINE_PER_CPU(uint64_t, cpu_user_rsp);

void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  __asm__ volatile("cpuid"
    : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
    : "a"(leaf));
}
EXPORT_SYMBOL(cpuid);

void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  __asm__ volatile("cpuid"
    : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
    : "a"(leaf), "c"(subleaf));
}
EXPORT_SYMBOL(cpuid_count);

// MSR access
uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t) high << 32) | low;
}
EXPORT_SYMBOL(rdmsr);

void wrmsr(uint32_t msr, uint64_t value) {
  uint32_t low = value & 0xFFFFFFFF;
  uint32_t high = value >> 32;
  __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}
EXPORT_SYMBOL(wrmsr);

irq_flags_t save_irq_flags(void) {
  irq_flags_t flags;
  __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
  return flags;
}
EXPORT_SYMBOL(save_irq_flags);

void restore_irq_flags(irq_flags_t flags) {
  __asm__ volatile("pushq %0\n\tpopfq" : : "r"(flags));
}
EXPORT_SYMBOL(restore_irq_flags);

irq_flags_t local_irq_save(void) {
  irq_flags_t f = save_irq_flags();
  cpu_cli();
  return f;
}
EXPORT_SYMBOL(local_irq_save);

void local_irq_restore(irq_flags_t flags) {
  restore_irq_flags(flags);
  cpu_sti();
}
EXPORT_SYMBOL(local_irq_restore);