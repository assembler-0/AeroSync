/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file include/arch/x64/fpu.h
 * @brief FPU/SSE/AVX state management
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 */

#pragma once

#include <kernel/types.h>

/* Maximum XSAVE area size - covers AVX-512 and future extensions */
#define XSTATE_MAX_SIZE 4096

/* XSAVE feature bits */
#define XFEATURE_MASK_FP (1ULL << 0)
#define XFEATURE_MASK_SSE (1ULL << 1)
#define XFEATURE_MASK_YMM (1ULL << 2) /* AVX */
#define XFEATURE_MASK_BNDREGS (1ULL << 3)
#define XFEATURE_MASK_BNDCSR (1ULL << 4)
#define XFEATURE_MASK_OPMASK (1ULL << 5) /* AVX-512 */
#define XFEATURE_MASK_ZMM_Hi256 (1ULL << 6)
#define XFEATURE_MASK_Hi16_ZMM (1ULL << 7)

/* Common feature combinations */
#define XFEATURE_MASK_FPSSE (XFEATURE_MASK_FP | XFEATURE_MASK_SSE)
#define XFEATURE_MASK_AVX (XFEATURE_MASK_FPSSE | XFEATURE_MASK_YMM)
#define XFEATURE_MASK_AVX512                                                   \
  (XFEATURE_MASK_AVX | XFEATURE_MASK_OPMASK | XFEATURE_MASK_ZMM_Hi256 |        \
   XFEATURE_MASK_Hi16_ZMM)

/**
 * struct fpu_state - FPU/SSE/AVX state storage
 *
 * This structure holds the extended CPU state for a task.
 * It must be 64-byte aligned for XSAVE.
 */
struct fpu_state {
  uint8_t state[XSTATE_MAX_SIZE] __attribute__((aligned(64)));
};

/**
 * fpu_init - Initialize FPU subsystem
 *
 * Detects XSAVE support and available features.
 * Called once during kernel boot.
 */
void fpu_init(void);

/**
 * fpu_init_task - Initialize FPU state for a new task
 * @fpu: FPU state structure to initialize
 *
 * Sets up default FPU state (clear all registers).
 */
void fpu_init_task(struct fpu_state *fpu);

/**
 * fpu_alloc - Allocate FPU state structure
 *
 * Returns: Pointer to allocated fpu_state, or NULL on failure
 */
struct fpu_state *fpu_alloc(void);

/**
 * fpu_free - Free FPU state structure
 * @fpu: FPU state to free
 */
void fpu_free(struct fpu_state *fpu);

/**
 * fpu_save - Save current CPU's FPU state
 * @fpu: Destination FPU state
 */
void fpu_save(struct fpu_state *fpu);

/**
 * fpu_restore - Restore FPU state to current CPU
 * @fpu: Source FPU state
 */
void fpu_restore(struct fpu_state *fpu);

/**
 * fpu_copy - Copy FPU state (for fork)
 * @dst: Destination FPU state
 * @src: Source FPU state
 */
void fpu_copy(struct fpu_state *dst, const struct fpu_state *src);

/**
 * cpu_has_xsave - Check if CPU supports XSAVE
 *
 * Returns: true if XSAVE is available
 */
bool cpu_has_xsave(void);

/**
 * cpu_has_fxsr - Check if CPU supports FXSAVE/FXRSTOR
 *
 * Returns: true if FXSAVE is available (always true on x86-64)
 */
bool cpu_has_fxsr(void);

/**
 * fpu_get_xstate_size - Get size of XSAVE area
 *
 * Returns: Size in bytes of the XSAVE area
 */
uint32_t fpu_get_xstate_size(void);

/**
 * fpu_get_xstate_mask - Get supported XSAVE feature mask
 *
 * Returns: Bitmask of supported XSAVE features
 */
uint64_t fpu_get_xstate_mask(void);
