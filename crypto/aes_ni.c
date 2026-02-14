/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/aes_ni.c
 * @brief AES using Intel AES-NI instructions
 * @copyright (C) 2025-2026 assembler-0
 */

#include <crypto/aes.h>
#include <aerosync/crypto.h>
#include <arch/x86_64/fpu.h>
#include <aerosync/errno.h>

#define AES_ALIGN __aligned(16)

__attribute__((target("aes,sse4.1")))
static inline __attribute__((always_inline)) void aes_ni_encrypt_block(const struct aes_ctx *ctx, uint8_t *dst, const uint8_t *src) {
  __asm__ volatile (
    "movdqu (%1), %%xmm0\n\t"        /* state = plaintext */
    "movdqa 0(%2), %%xmm1\n\t"       /* round key 0 */
    "pxor %%xmm1, %%xmm0\n\t"        /* state ^= round key 0 */
    
    "movdqa 16(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 32(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 48(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 64(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 80(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 96(%2), %%xmm1\n\t"  "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 112(%2), %%xmm1\n\t" "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 128(%2), %%xmm1\n\t" "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 144(%2), %%xmm1\n\t" "aesenc %%xmm1, %%xmm0\n\t"
    "movdqa 160(%2), %%xmm1\n\t" "aesenclast %%xmm1, %%xmm0\n\t"
    
    "movdqu %%xmm0, (%0)\n\t"        /* ciphertext = state */
    : : "r" (dst), "r" (src), "r" (ctx->key_enc) : "xmm0", "xmm1", "memory"
  );
}

__attribute__((target("aes,sse4.1")))
static int crypto_aes_ni_encrypt(void *ctx, uint8_t *dst, const uint8_t *src) {
  kernel_fpu_begin();
  aes_ni_encrypt_block(ctx, dst, src);
  kernel_fpu_end();
  return 0;
}

/* round key generation helper */
#define AES_128_KEY_EXP(k, rcon) \
  __asm__ volatile ( \
    "aeskeygenassist %2, %1, %%xmm2\n\t" \
    "pshufd $0xff, %%xmm2, %%xmm2\n\t" \
    "movdqa %1, %%xmm3\n\t" \
    "pslldq $4, %%xmm3\n\t" \
    "pxor %%xmm3, %1\n\t" \
    "pslldq $4, %%xmm3\n\t" \
    "pxor %%xmm3, %1\n\t" \
    "pslldq $4, %%xmm3\n\t" \
    "pxor %%xmm3, %1\n\t" \
    "pxor %%xmm2, %1\n\t" \
    : "+x" (k) : "x" (k), "i" (rcon) : "xmm2", "xmm3" \
  )

__attribute__((target("aes,sse4.1")))
static int aes_ni_set_key(struct aes_ctx *ctx, const uint8_t *in_key, size_t key_len) {
  if (key_len != 16) return -EINVAL; /* 128-bit only for now */
  
  return aes_set_key(ctx, in_key, key_len); /* Fallback to generic for key expansion logic */
}

static struct crypto_alg aes_ni_alg = {
  .name = "aes",
  .driver_name = "aes-ni",
  .priority = 300,
  .type = CRYPTO_ALG_TYPE_CIPHER,
  .ctx_size = sizeof(struct aes_ctx),
  .cipher = {
    .min_keysize = 16,
    .max_keysize = 16,
    .blocksize = 16,
    .setkey = (void*)aes_ni_set_key,
    .encrypt = crypto_aes_ni_encrypt,
    .decrypt = crypto_aes_ni_encrypt, /* Stub */
  },
};

int aes_ni_init(void) {
  if (crypto_has_aes_ni()) {
    return crypto_register_alg(&aes_ni_alg);
  }
  return 0;
}
