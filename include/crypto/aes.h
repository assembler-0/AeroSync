#pragma once

#include <aerosync/types.h>

#define AES_MIN_KEY_SIZE	16
#define AES_MAX_KEY_SIZE	32
#define AES_KEYSIZE_128		16
#define AES_KEYSIZE_192		24
#define AES_KEYSIZE_256		32
#define AES_BLOCK_SIZE		16

struct aes_ctx {
    uint32_t key_enc[60];
    uint32_t key_dec[60];
    int rounds;
};

int aes_set_key(struct aes_ctx *ctx, const uint8_t *in_key, size_t key_len);
void aes_encrypt(struct aes_ctx *ctx, uint8_t *out, const uint8_t *in);
void aes_decrypt(struct aes_ctx *ctx, uint8_t *out, const uint8_t *in);
