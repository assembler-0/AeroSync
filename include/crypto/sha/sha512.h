#ifndef CRYPTO_SHA512_H
#define CRYPTO_SHA512_H

#include <aerosync/types.h>

typedef struct {
    uint64_t state[8];
    uint64_t count[2];
    uint8_t buffer[128];
} sha512_context;

void sha512_init(sha512_context * ctx);
void sha512_update(sha512_context * ctx, const uint8_t * data, size_t len);
void sha512_final(sha512_context * ctx, uint8_t * hash);
void sha512(const uint8_t * data, size_t len, uint8_t * hash);

#endif
