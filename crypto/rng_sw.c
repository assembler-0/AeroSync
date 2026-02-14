/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/rng_sw.c
 * @brief Software PRNG (xoroshiro128+)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <arch/x86_64/tsc.h>
#include <lib/string.h>

static uint64_t state[2] = { 0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL };

static inline uint64_t rotl(const uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

static int sw_rng_generate(void *ctx, uint8_t *dst, size_t len) {
  (void)ctx;
  size_t i = 0;
  while (i < len) {
    uint64_t s0 = state[0];
    uint64_t s1 = state[1];
    uint64_t result = s0 + s1;
        
    s1 ^= s0;
    state[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16);
    state[1] = rotl(s1, 37);
        
    size_t chunk = (len - i < 8) ? (len - i) : 8;
    for (size_t j = 0; j < chunk; j++) {
      dst[i++] = (result >> (j * 8)) & 0xff;
    }
  }
  return 0;
}

static int sw_rng_seed(void *ctx, const uint8_t *seed, size_t len) {
  (void)ctx;
  if (len >= 16) {
    memcpy(state, seed, 16);
  } else {
    memcpy(state, seed, len);
  }
  return 0;
}

static struct crypto_alg sw_rng_alg = {
  .name = "sw_rng",
  .driver_name = "xoroshiro128plus",
  .priority = 100,
  .type = CRYPTO_ALG_TYPE_RNG,
  .ctx_size = 0,
  .rng = {
    .generate = sw_rng_generate,
    .seed = sw_rng_seed,
  },
};

int sw_rng_init(void) {
  state[0] ^= rdtsc();
  return crypto_register_alg(&sw_rng_alg);
}
