/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/crc32.c
 * @brief CRC32 implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <aerosync/compiler.h>
#include <crypto/crc32.h>
#include <aerosync/crypto.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

static alignas(16) uint32_t crc32_table[256];

void crc32_init() {
    uint32_t polynomial = 0xEDB88320;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (size_t j = 0; j < 8; j++) {
            if (c & 1) {
                c = polynomial ^ (c >> 1);
            } else {
                c >>= 1;
            }
        }
        crc32_table[i] = c;
    }
}

uint32_t crc32(const void* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = (const uint8_t*)data;

    for (size_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

static int crypto_crc32_init(void *ctx) {
    *(uint32_t *)ctx = 0xFFFFFFFF;
    return 0;
}

static int crypto_crc32_update(void *ctx, const uint8_t *data, size_t len) {
    uint32_t *crc = ctx;
    for (size_t i = 0; i < len; i++) {
        *crc = crc32_table[(*crc ^ data[i]) & 0xFF] ^ (*crc >> 8);
    }
    return 0;
}

static int crypto_crc32_final(void *ctx, uint8_t *out) {
    uint32_t *crc = ctx;
    *(uint32_t *)out = *crc ^ 0xFFFFFFFF;
    return 0;
}

static struct crypto_alg crc32_alg = {
    .name = "crc32",
    .driver_name = "crc32-generic",
    .priority = 100,
      .type = CRYPTO_ALG_TYPE_SHASH,
      .ctx_size = sizeof(uint32_t),
      .init = crypto_crc32_init,
      .shash = {
        .digestsize = 4,
        .blocksize = 1,
        .update = crypto_crc32_update,
        .final = crypto_crc32_final,
      },
    };
    int __init crc32_generic_init(void) {
    crc32_init();
    return crypto_register_alg(&crc32_alg);
}