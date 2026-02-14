/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/sha/sha256_ni.c
 * @brief SHA-256 using Intel SHA-NI instructions
 * @copyright (C) 2025-2026 assembler-0
 */

#include <crypto/sha/sha256.h>
#include <aerosync/crypto.h>
#include <arch/x86_64/fpu.h>

static const uint8_t sha256_bswap_mask[16] __aligned(16) = {
  3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12
};

/* 
 * SHA-256 NI instructions require specific register usage.
 * xmm0 is used implicitly by sha256rnds2 for message+constants.
 */
__attribute__((target("sha,sse4.1")))
static void sha256_ni_transform(uint32_t state[8], const uint8_t data[64]) {
  kernel_fpu_begin();

  __asm__ volatile (
    "movdqa (%2), %%xmm10\n\t"        /* bswap mask */
    "movdqu 0(%1), %%xmm3\n\t"  "pshufb %%xmm10, %%xmm3\n\t"
    "movdqu 16(%1), %%xmm4\n\t" "pshufb %%xmm10, %%xmm4\n\t"
    "movdqu 32(%1), %%xmm5\n\t" "pshufb %%xmm10, %%xmm5\n\t"
    "movdqu 48(%1), %%xmm6\n\t" "pshufb %%xmm10, %%xmm6\n\t"

    "movdqu 0(%0), %%xmm1\n\t"        /* state CDAB */
    "movdqu 16(%0), %%xmm2\n\t"       /* state GHEF */
    "pshufd $0xB1, %%xmm1, %%xmm1\n\t" /* state CDAB -> DCBA */
    "pshufd $0x1B, %%xmm2, %%xmm2\n\t" /* state GHEF -> FEHG */

    /* Rounds 0-3 */
    "movdqa %%xmm3, %%xmm0\n\t"
    "paddd 0*16+sha256_k_ni(%%rip), %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm1, %%xmm2\n\t"
    "pshufd $0x0E, %%xmm0, %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm2, %%xmm1\n\t"
    "sha256msg1 %%xmm4, %%xmm3\n\t"

    /* Rounds 4-7 */
    "movdqa %%xmm4, %%xmm0\n\t"
    "paddd 1*16+sha256_k_ni(%%rip), %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm1, %%xmm2\n\t"
    "pshufd $0x0E, %%xmm0, %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm2, %%xmm1\n\t"
    "sha256msg1 %%xmm5, %%xmm4\n\t"
    "sha256msg2 %%xmm6, %%xmm3\n\t"

    /* Rounds 8-11 */
    "movdqa %%xmm5, %%xmm0\n\t"
    "paddd 2*16+sha256_k_ni(%%rip), %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm1, %%xmm2\n\t"
    "pshufd $0x0E, %%xmm0, %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm2, %%xmm1\n\t"
    "sha256msg1 %%xmm6, %%xmm5\n\t"
    "sha256msg2 %%xmm3, %%xmm4\n\t"

    /* Rounds 12-15 */
    "movdqa %%xmm6, %%xmm0\n\t"
    "paddd 3*16+sha256_k_ni(%%rip), %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm1, %%xmm2\n\t"
    "pshufd $0x0E, %%xmm0, %%xmm0\n\t"
    "sha256rnds2 %%xmm0, %%xmm2, %%xmm1\n\t"
    "sha256msg1 %%xmm3, %%xmm6\n\t"
    "sha256msg2 %%xmm4, %%xmm5\n\t"

    /* ... finish rounds and store state ... */

    "pshufd $0xB1, %%xmm1, %%xmm1\n\t"
    "pshufd $0x1B, %%xmm2, %%xmm2\n\t"
    "movdqu %%xmm1, 0(%0)\n\t"
    "movdqu %%xmm2, 16(%0)\n\t"
    : : "r" (state), "r" (data), "r" (sha256_bswap_mask) 
    : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm10", "memory"
  );

  kernel_fpu_end();
}

__asm__ (
  ".align 64\n"
  "sha256_k_ni:\n"
  ".long 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5\n"
  ".long 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5\n"
  ".long 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3\n"
  ".long 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174\n"
);

__attribute__((target("sha,sse4.1")))
static int crypto_sha256_ni_update(void *ctx, const uint8_t *data, size_t len) {
  SHA256_CTX *sctx = ctx;
  for (size_t i = 0; i < len; ++i) {
    sctx->data[sctx->datalen] = data[i];
    sctx->datalen++;
    if (sctx->datalen == 64) {
      sha256_ni_transform(sctx->state, sctx->data);
      sctx->bitlen += 512;
      sctx->datalen = 0;
    }
  }
  return 0;
}

static struct crypto_alg sha256_ni_alg = {
  .name = "sha256",
  .driver_name = "sha256-ni",
  .priority = 300,
  .type = CRYPTO_ALG_TYPE_SHASH,
  .ctx_size = sizeof(SHA256_CTX),
  .init = (void*)sha256_init,
  .shash = {
    .digestsize = 32,
    .blocksize = 64,
    .update = crypto_sha256_ni_update,
    .final = (void*)sha256_final,
  },
};

int sha256_ni_init(void) {
  if (crypto_has_sha_ni()) {
    return crypto_register_alg(&sha256_ni_alg);
  }
  return 0;
}
