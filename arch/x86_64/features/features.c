/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/features/features.c
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
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/features/features.h>
#include <lib/printk.h>

static cpu_features_t g_cpu_features;

// CR0 bits
#define CR0_MP (1 << 1)
#define CR0_EM (1 << 2)
#define CR0_TS (1 << 3)
#define CR0_WP (1 << 16)

// CR4 bits
#define CR4_OSFXSR (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)
#define CR4_UMIP (1 << 11)
#define CR4_LA57 (1 << 12)
#define CR4_FSGSBASE (1 << 16)
#define CR4_PCIDE (1 << 17)
#define CR4_OSXSAVE (1 << 18)
#define CR4_SMEP (1 << 20)
#define CR4_SMAP (1 << 21)
#define CR4_PKE (1 << 22)
#define CR4_CET (1 << 23)

// XCR0 bits
#define XCR0_SSE (1 << 1)
#define XCR0_AVX (1 << 2)
#define XCR0_OPMASK (1 << 5)
#define XCR0_ZMM_HI256 (1 << 6)
#define XCR0_HI16_ZMM (1 << 7)

#define MSR_IA32_PAT 0x277
#define MSR_IA32_EFER 0xC0000080
#define MSR_IA32_U_CET 0x6A2
#define EFER_NXE (1 << 11)

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

static void pat_init(void) {
  if (!g_cpu_features.pat)
    return;

  // PAT layout:
  // 0: WB (06) - Default
  // 1: WC (01) - Standard default (PWT)
  // 2: UC- (07) - Standard default (PCD)
  // 3: UC (00) - Standard default (PWT | PCD)
  // 4: WB (06) - PAT
  // 5: WT (04) - PAT | PWT
  // 6: WC (01) - PAT | PCD
  // 7: WP (05) - PAT | PWT | PCD
  uint64_t pat = (0x06ULL << 0) | (0x01ULL << 8) | (0x07ULL << 16) |
                 (0x00ULL << 24) | (0x06ULL << 32) | (0x04ULL << 40) |
                 (0x01ULL << 48) | (0x05ULL << 56);
  wrmsr(MSR_IA32_PAT, pat);
}

void cpu_features_init_ap(void) {
  // Enable NX
  if (g_cpu_features.nx) {
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_IA32_EFER, efer);
  }

  // enable WP
  if (g_cpu_features.wp) {
    write_cr0(read_cr0() | CR0_WP);
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
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

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

  // Enable PCID
  if (g_cpu_features.pcid) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_PCIDE;
    write_cr4(cr4);
  }

  // Enable SMEP
  if (g_cpu_features.smep) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;
    write_cr4(cr4);
  }

  // Enable SMAP
  if (g_cpu_features.smap) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMAP;
    write_cr4(cr4);
  }

  // enable CET SS (shadow stack)
  // if (g_cpu_features.cet_ss) {
  //   write_cr4(read_cr4() | CR4_CET);
  //   uint32_t cet_u_lo = (1 << 0) | (1 << 1);
  //   wrmsr(MSR_IA32_U_CET, cet_u_lo);
  // }
  //
  // // enable PKE
  // if (g_cpu_features.pke) {
  //   write_cr4(read_cr4() | CR4_PKE);
  // }
  //
  // // enable UMIP
  // if (g_cpu_features.umip) {
  //   write_cr4(read_cr4() | CR4_UMIP);
  // }

  pat_init();
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
    if (edx & (1 << 16))
      g_cpu_features.pat = true;

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
      g_cpu_features.osxsave = true;
    if (ecx & (1 << 28))
      g_cpu_features.avx = true;

    if (ecx & (1 << 12))
      g_cpu_features.fma = true;
    if (ecx & (1 << 17))
      g_cpu_features.pcid = true;
  }

  if (max_leaf >= 7) {
    cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

    if (ebx & (1 << 3))
      g_cpu_features.bmi1 = true;
    if (ebx & (1 << 5))
      g_cpu_features.avx2 = true;
    if (ebx & (1 << 7))
      g_cpu_features.smep = true;
    if (ebx & (1 << 8))
      g_cpu_features.bmi2 = true;
    if (ebx & (1 << 10))
      g_cpu_features.invpcid = true;
    if (ebx & (1 << 16))
      g_cpu_features.avx512f = true;
    if (ebx & (1 << 20))
      g_cpu_features.smap = true;
    if (ecx & (1 << 16))
      g_cpu_features.la57 = true;
    if (ecx & (1 << 2))
      g_cpu_features.umip = true;
    if (ecx & (1 << 3))
      g_cpu_features.pke = true;
    if (ebx & (1 << 0))
      g_cpu_features.fsgsbase = true;
    if (ecx & (1 << 7))
      g_cpu_features.cet_ss = true;
  }

  // Check extended features
  cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
  if (eax >= 0x80000001) {
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (edx & (1 << 20))
      g_cpu_features.nx = true;
    if (edx & (1 << 26))
      g_cpu_features.pdpe1gb = true;
  }

  // Enable NX
  if (g_cpu_features.nx) {
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_IA32_EFER, efer);
  }

  write_cr0(read_cr0() | CR0_WP);
  g_cpu_features.wp = true;

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

  // Enable PCID
  if (g_cpu_features.pcid) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_PCIDE;
    write_cr4(cr4);
  }

  // Enable SMEP
  if (g_cpu_features.smep) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;
    write_cr4(cr4);
  }

  // Enable SMAP
  if (g_cpu_features.smap) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMAP;
    write_cr4(cr4);
  }

  // enable CET SS (shadow stack)
  // if (g_cpu_features.cet_ss) {
  //   write_cr4(read_cr4() | CR4_CET);
  //   uint32_t cet_u_lo = (1 << 0) | (1 << 1);
  //   wrmsr(MSR_IA32_U_CET, cet_u_lo);
  // }
  //
  // // enable PKE
  // if (g_cpu_features.pke) {
  //   write_cr4(read_cr4() | CR4_PKE);
  // }
  //
  // // enable UMIP
  // if (g_cpu_features.umip) {
  //   write_cr4(read_cr4() | CR4_UMIP);
  // }

  pat_init();

  cpu_features_dump(&g_cpu_features);
}

void cpu_features_dump(cpu_features_t *features) {
  if (!features)
    features = &g_cpu_features;

  printk(CPU_CLASS "system processor capabilities (PC):\n");
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
  printk(CPU_CLASS "  PAT: %s\n", features->pat ? "Yes" : "No");
  printk(CPU_CLASS "  LA57: %s\n", features->la57 ? "Yes" : "No");
  printk(CPU_CLASS "  1GB Pages: %s\n", features->pdpe1gb ? "Yes" : "No");
  printk(CPU_CLASS "  NX: %s\n", features->nx ? "Yes" : "No");
  printk(CPU_CLASS "  WP: %s\n", features->wp ? "Yes" : "No");
  printk(CPU_CLASS "  PCID: %s\n", features->pcid ? "Yes" : "No");
  printk(CPU_CLASS "  INVPCID: %s\n", features->invpcid ? "Yes" : "No");
  printk(CPU_CLASS "  SMEP: %s\n", features->smep ? "Yes" : "No");
  printk(CPU_CLASS "  SMAP: %s\n", features->smap ? "Yes" : "No");
  printk(CPU_CLASS "  UMIP: %s\n", features->umip ? "Yes" : "No");
  printk(CPU_CLASS "  PKE: %s\n", features->pke ? "Yes" : "No");
  printk(CPU_CLASS "  CET: %s\n", features->cet_ss ? "Yes" : "No");
}

cpu_features_t *get_cpu_features(void) {
  return &g_cpu_features;
}
