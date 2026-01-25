#ifndef ED25519_H
#define ED25519_H

#include <stddef.h>
#include <stdint.h>

void ed25519_create_keypair(uint8_t *public_key, uint8_t *private_key, const uint8_t *seed);
void ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key, const uint8_t *private_key);
int ed25519_verify(const uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key);
void ed25519_create_seed(uint8_t *seed);

#endif
