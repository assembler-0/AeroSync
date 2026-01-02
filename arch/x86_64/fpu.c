/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/fpu.c
 * @brief FPU/SSE/AVX state management implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * Implements lazy FPU state saving/restoring for context switches.
 * Supports FXSAVE/FXRSTOR and XSAVE/XRSTOR depending on CPU features.
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/fpu.h>
#include <kernel/classes.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>


/* CPU capability flags */
static bool has_xsave = false;
static bool has_xsaveopt = false;
static bool has_fxsr = true; /* Always available on x86-64 */

/* XSAVE state info */
static uint64_t xstate_mask = 0;
static uint32_t xstate_size = 512; /* Default FXSAVE size */

/* CPUID feature bits */
#define CPUID_01_ECX_XSAVE (1 << 26)
#define CPUID_01_ECX_OSXSAVE (1 << 27)
#define CPUID_0D_01_EAX_XSAVEOPT (1 << 0)

/* CR0 bits */
#define CR0_EM (1 << 2) /* Emulation */
#define CR0_TS (1 << 3) /* Task switched */
#define CR0_MP (1 << 1) /* Monitor coprocessor */

/* CR4 bits */
#define CR4_OSFXSR (1 << 9)      /* Enable FXSAVE/FXRSTOR */
#define CR4_OSXMMEXCPT (1 << 10) /* Enable unmasked SSE exceptions */
#define CR4_OSXSAVE (1 << 18)    /* Enable XSAVE */

/* XCR0 bits */
#define XCR0_X87 (1 << 0)
#define XCR0_SSE (1 << 1)
#define XCR0_AVX (1 << 2)

static inline uint64_t read_cr0(void) {
  uint64_t val;
  __asm__ volatile("mov %%cr0, %0" : "=r"(val));
  return val;
}

static inline void write_cr0(uint64_t val) {
  __asm__ volatile("mov %0, %%cr0" : : "r"(val));
}

static inline uint64_t read_cr4(void) {
  uint64_t val;
  __asm__ volatile("mov %%cr4, %0" : "=r"(val));
  return val;
}

static inline void write_cr4(uint64_t val) {
  __asm__ volatile("mov %0, %%cr4" : : "r"(val));
}

static inline uint64_t xgetbv(uint32_t index) {
  uint32_t eax, edx;
  __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
  return ((uint64_t)edx << 32) | eax;
}

static inline void xsetbv(uint32_t index, uint64_t value) {
  uint32_t eax = (uint32_t)value;
  uint32_t edx = (uint32_t)(value >> 32);
  __asm__ volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(index));
}

static inline void fxsave(void *state) {
  __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}

static inline void fxrstor(const void *state) {
  __asm__ volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}

static inline void xsave(void *state, uint64_t mask) {
  uint32_t eax = (uint32_t)mask;
  uint32_t edx = (uint32_t)(mask >> 32);
  __asm__ volatile("xsave64 (%0)"
                   :
                   : "r"(state), "a"(eax), "d"(edx)
                   : "memory");
}

static inline void xrstor(const void *state, uint64_t mask) {
  uint32_t eax = (uint32_t)mask;
  uint32_t edx = (uint32_t)(mask >> 32);
  __asm__ volatile("xrstor64 (%0)"
                   :
                   : "r"(state), "a"(eax), "d"(edx)
                   : "memory");
}

static inline void xsaveopt(void *state, uint64_t mask) {
  uint32_t eax = (uint32_t)mask;
  uint32_t edx = (uint32_t)(mask >> 32);
  __asm__ volatile("xsaveopt64 (%0)"
                   :
                   : "r"(state), "a"(eax), "d"(edx)
                   : "memory");
}

void fpu_init(void) {
  uint32_t eax, ebx, ecx, edx;
  uint64_t cr0, cr4;

  /* Enable FPU and SSE in CR0 */
  cr0 = read_cr0();
  cr0 &= ~CR0_EM; /* Clear emulation */
  cr0 |= CR0_MP;  /* Set monitor coprocessor */
  cr0 &= ~CR0_TS; /* Clear task switched (for now) */
  write_cr0(cr0);

  /* Enable FXSAVE/FXRSTOR and SSE exceptions in CR4 */
  cr4 = read_cr4();
  cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

  /* Check for XSAVE support */
  cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
  if (ecx & CPUID_01_ECX_XSAVE) {
    /* Enable XSAVE in CR4 */
    cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    has_xsave = true;

    /* Get supported XSAVE features from XCR0 */
    xstate_mask = xgetbv(0);

    /* Enable all available features in XCR0 */
    cpuid_count(0x0D, 0, &eax, &ebx, &ecx, &edx);
    uint64_t supported = ((uint64_t)edx << 32) | eax;
    xstate_size = ebx; /* Total size needed */

    /* Always enable x87 and SSE */
    uint64_t enable_mask = XCR0_X87 | XCR0_SSE;

    /* Enable AVX if supported */
    if (supported & XCR0_AVX) {
      enable_mask |= XCR0_AVX;
    }

    xsetbv(0, enable_mask);
    xstate_mask = xgetbv(0);

    /* Check for XSAVEOPT */
    cpuid_count(0x0D, 1, &eax, &ebx, &ecx, &edx);
    if (eax & CPUID_0D_01_EAX_XSAVEOPT) {
      has_xsaveopt = true;
    }

    printk(KERN_DEBUG FPU_CLASS
           "XSAVE enabled, features=0x%llx size=%u xsaveopt=%s\n",
           xstate_mask, xstate_size, has_xsaveopt ? "yes" : "no");
  } else {
    write_cr4(cr4);
    printk(KERN_DEBUG FPU_CLASS "Using FXSAVE (no XSAVE support)\n");
  }

  /* Initialize FPU */
  __asm__ volatile("fninit");
}

void fpu_init_task(struct fpu_state *fpu) {
  if (!fpu)
    return;

  memset(fpu->state, 0, XSTATE_MAX_SIZE);

  /* Set up default x87 FPU control word (double precision, all exceptions
   * masked) */
  /* FXSAVE format: offset 0 = FCW (FPU Control Word) */
  uint16_t *fcw = (uint16_t *)&fpu->state[0];
  *fcw = 0x037F; /* All exceptions masked, double precision */

  /* MXCSR at offset 24 in FXSAVE area */
  uint32_t *mxcsr = (uint32_t *)&fpu->state[24];
  *mxcsr = 0x1F80; /* Default MXCSR: all exceptions masked */

  if (has_xsave) {
    /* XSAVE header at offset 512 */
    uint64_t *xstate_bv = (uint64_t *)&fpu->state[512];
    *xstate_bv = XFEATURE_MASK_FPSSE; /* Mark FP and SSE as valid */
  }
}

struct fpu_state *fpu_alloc(void) {
  /* Allocate 64-byte aligned memory */
  struct fpu_state *fpu = kmalloc(sizeof(struct fpu_state));
  if (fpu) {
    fpu_init_task(fpu);
  }
  return fpu;
}

void fpu_free(struct fpu_state *fpu) {
  if (fpu) {
    kfree(fpu);
  }
}

void fpu_save(struct fpu_state *fpu) {
  if (!fpu)
    return;

  if (has_xsave) {
    if (has_xsaveopt) {
      xsaveopt(fpu->state, xstate_mask);
    } else {
      xsave(fpu->state, xstate_mask);
    }
  } else {
    fxsave(fpu->state);
  }
}

void fpu_restore(struct fpu_state *fpu) {
  if (!fpu)
    return;

  if (has_xsave) {
    xrstor(fpu->state, xstate_mask);
  } else {
    fxrstor(fpu->state);
  }
}

void fpu_copy(struct fpu_state *dst, const struct fpu_state *src) {
  if (!dst || !src)
    return;

  memcpy(dst->state, src->state, xstate_size);
}

bool cpu_has_xsave(void) { return has_xsave; }

bool cpu_has_fxsr(void) { return has_fxsr; }

uint32_t fpu_get_xstate_size(void) { return xstate_size; }

uint64_t fpu_get_xstate_mask(void) { return xstate_mask; }
