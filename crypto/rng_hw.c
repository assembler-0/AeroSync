/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/rng_hw.c
 * @brief Hardware RNG using RDRAND/RDSEED
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <arch/x86_64/cpu.h>

static int hw_rng_generate(void *ctx, uint8_t *dst, size_t len) {
  (void)ctx;
  size_t i = 0;
    
  while (i < len) {
    uint64_t val;
    unsigned char ok;
        
    /* Try RDSEED first if available */
    if (crypto_has_rdseed()) {
      __asm__ volatile ("rdseed %0; setc %1" : "=r" (val), "=qm" (ok));
    } else {
      __asm__ volatile ("rdrand %0; setc %1" : "=r" (val), "=qm" (ok));
    }
        
    if (ok) {
      size_t chunk = (len - i < 8) ? (len - i) : 8;
      for (size_t j = 0; j < chunk; j++) {
        dst[i++] = (val >> (j * 8)) & 0xff;
      }
    } else {
      cpu_relax();
    }
  }
  return 0;
}

static struct crypto_alg hw_rng_alg = {
  .name = "hw_rng",
  .driver_name = "intel_rdrand",
  .priority = 200,
  .type = CRYPTO_ALG_TYPE_RNG,
  .ctx_size = 0,
  .rng = {
    .generate = hw_rng_generate,
  },
};

int hw_rng_init(void) {
  if (crypto_has_rdrand() || crypto_has_rdseed()) {
    return crypto_register_alg(&hw_rng_alg);
  }
  return 0;
}
