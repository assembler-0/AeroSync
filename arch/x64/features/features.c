/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/features/features.c
 * @brief CPU feature detection and enabling for x86_64 architecture
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

#include <kernel/classes.h>
#include <arch/x64/cpu.h>
#include <arch/x64/features/features.h>
#include <lib/printk.h>

static cpu_features_t g_cpu_features;

// CR0 bits
#define CR0_MP (1 << 1)
#define CR0_EM (1 << 2)

// CR4 bits
#define CR4_OSFXSR (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)
#define CR4_OSXSAVE (1 << 18)

// XCR0 bits
#define XCR0_SSE (1 << 1)
#define XCR0_AVX (1 << 2)
#define XCR0_OPMASK (1 << 5)
#define XCR0_ZMM_HI256 (1 << 6)
#define XCR0_HI16_ZMM (1 << 7)

static inline void xsetbv(uint32_t reg, uint64_t value) {
  uint32_t lo = value & 0xFFFFFFFF;
  uint32_t hi = value >> 32;
  __asm__ volatile("xsetbv" ::"c"(reg), "a"(lo), "d"(hi));
}

static inline uint64_t xgetbv(uint32_t reg) {
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(reg));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t read_cr0(void) {
  uint64_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  return cr0;
}

static void write_cr0(uint64_t cr0) {
  __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
}

static uint64_t read_cr4(void) {
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  return cr4;
}

static void write_cr4(uint64_t cr4) {
  __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));
}

void cpu_features_init(void) {
  uint32_t eax, ebx, ecx, edx;

  // Check max leaf
  cpuid(0, &eax, &ebx, &ecx, &edx);
  uint32_t max_leaf = eax;

  if (max_leaf >= 1) {
    cpuid(1, &eax, &ebx, &ecx, &edx);

    if (edx & (1 << 25))
      g_cpu_features.sse = true;
    if (edx & (1 << 26))
      g_cpu_features.sse2 = true;

    if (ecx & (1 << 0))
      g_cpu_features.sse3 = true;
    if (ecx & (1 << 9))
      g_cpu_features.ssse3 = true;
    if (ecx & (1 << 19))
      g_cpu_features.sse41 = true;
    if (ecx & (1 << 20))
      g_cpu_features.sse42 = true;

    if (ecx & (1 << 26))
      g_cpu_features.xsave = true;
    if (ecx & (1 << 27))
      g_cpu_features.osxsave = true; // Should be 0 initially
    if (ecx & (1 << 28))
      g_cpu_features.avx = true;

    if (ecx & (1 << 12))
      g_cpu_features.fma = true;
  }

  if (max_leaf >= 7) {
    cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

    if (ebx & (1 << 3))
      g_cpu_features.bmi1 = true;
    if (ebx & (1 << 5))
      g_cpu_features.avx2 = true;
    if (ebx & (1 << 8))
      g_cpu_features.bmi2 = true;
    if (ebx & (1 << 16))
      g_cpu_features.avx512f = true;
  }

  // Enable SSE
  if (g_cpu_features.sse) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);
  }

  // Enable AVX
  if (g_cpu_features.avx && g_cpu_features.xsave) {
    // Enable OSXSAVE in CR4
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    // Update features to reflect we enabled it
    g_cpu_features.osxsave = true;

    // Enable SSE and AVX in XCR0
    // We must fetch current XCR0, but XGETBV is only valid if CR4.OSXSAVE is
    // set. We just set it.
    uint64_t xcr0 = xgetbv(0);
    xcr0 |= XCR0_SSE | XCR0_AVX;
    xsetbv(0, xcr0);
  }

  // Enable AVX512
  if (g_cpu_features.avx512f && g_cpu_features.osxsave) {
    uint64_t xcr0 = xgetbv(0);
    xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
    xsetbv(0, xcr0);
  }

  cpu_features_dump(&g_cpu_features);
}

void cpu_features_dump(cpu_features_t *features) {
  if (!features)
    features = &g_cpu_features;

  printk(CPU_CLASS "CPU Features:\n");
  printk(CPU_CLASS "  SSE: %s\n", features->sse ? "Yes" : "No");
  printk(CPU_CLASS "  SSE2: %s\n", features->sse2 ? "Yes" : "No");
  printk(CPU_CLASS "  SSE3: %s\n", features->sse3 ? "Yes" : "No");
  printk(CPU_CLASS "  SSSE3: %s\n", features->ssse3 ? "Yes" : "No");
  printk(CPU_CLASS "  SSE4.1: %s\n", features->sse41 ? "Yes" : "No");
  printk(CPU_CLASS "  SSE4.2: %s\n", features->sse42 ? "Yes" : "No");
  printk(CPU_CLASS "  XSAVE: %s\n", features->xsave ? "Yes" : "No");
  printk(CPU_CLASS "  OSXSAVE: %s\n", features->osxsave ? "Yes" : "No");
  printk(CPU_CLASS "  AVX: %s\n", features->avx ? "Yes" : "No");
  printk(CPU_CLASS "  AVX2: %s\n", features->avx2 ? "Yes" : "No");
  printk(CPU_CLASS "  AVX512F: %s\n", features->avx512f ? "Yes" : "No");
  printk(CPU_CLASS "  FMA: %s\n", features->fma ? "Yes" : "No");
  printk(CPU_CLASS "  BMI1: %s\n", features->bmi1 ? "Yes" : "No");
  printk(CPU_CLASS "  BMI2: %s\n", features->bmi2 ? "Yes" : "No");
}

cpu_features_t *get_cpu_features(void) {
  return &g_cpu_features;
}
