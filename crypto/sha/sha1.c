/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/sha/sha1.c
 * @brief SHA-1 implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <lib/string.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e, i;
    uint32_t w[80];

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) | (uint32_t)buffer[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 20; i++) {
        uint32_t t = rol(a, 5) + ((b & c) | (~b & d)) + e + w[i] + 0x5A827999;
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    for (; i < 40; i++) {
        uint32_t t = rol(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    for (; i < 60; i++) {
        uint32_t t = rol(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDC;
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    for (; i < 80; i++) {
        uint32_t t = rol(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static int sha1_init(void *ctx) {
    SHA1_CTX *sctx = ctx;
    sctx->state[0] = 0x67452301;
    sctx->state[1] = 0xEFCDAB89;
    sctx->state[2] = 0x98BADCFE;
    sctx->state[3] = 0x10325476;
    sctx->state[4] = 0xC3D2E1F0;
    sctx->count[0] = sctx->count[1] = 0;
    return 0;
}

static int sha1_update(void *ctx, const uint8_t *data, size_t len) {
    SHA1_CTX *sctx = ctx;
    size_t i, j;

    j = (sctx->count[0] >> 3) & 63;
    if ((sctx->count[0] += (uint32_t)len << 3) < ((uint32_t)len << 3)) sctx->count[1]++;
    sctx->count[1] += (uint32_t)(len >> 29);

    if ((j + len) > 63) {
        memcpy(&sctx->buffer[j], data, (i = 64 - j));
        sha1_transform(sctx->state, sctx->buffer);
        for (; i + 63 < len; i += 64) sha1_transform(sctx->state, &data[i]);
        j = 0;
    } else i = 0;
    memcpy(&sctx->buffer[j], &data[i], len - i);
    return 0;
}

static int sha1_final(void *ctx, uint8_t *out) {
    SHA1_CTX *sctx = ctx;
    uint32_t i;
    uint8_t finalcount[8];

    for (i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((sctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    sha1_update(sctx, (const uint8_t *)"\200", 1);
    while ((sctx->count[0] & 504) != 448) sha1_update(sctx, (const uint8_t *)"\0", 1);
    sha1_update(sctx, finalcount, 8);

    for (i = 0; i < 20; i++) {
        out[i] = (uint8_t)((sctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
    return 0;
}

static struct crypto_alg sha1_alg = {
    .name = "sha1",
    .driver_name = "sha1-generic",
    .priority = 100,
      .type = CRYPTO_ALG_TYPE_SHASH,
      .ctx_size = sizeof(SHA1_CTX),
      .init = sha1_init,
      .shash = {
        .digestsize = 20,
        .blocksize = 64,
        .update = sha1_update,
        .final = sha1_final,
      },
    };
    int __init sha1_generic_init(void) {
    return crypto_register_alg(&sha1_alg);
}
