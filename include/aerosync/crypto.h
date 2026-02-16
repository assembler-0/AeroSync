/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/crypto.h
 * @brief Core Cryptography API
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <linux/list.h>

#define CRYPTO_MAX_ALG_NAME 64

enum crypto_alg_type {
  CRYPTO_ALG_TYPE_SHASH,
  CRYPTO_ALG_TYPE_CIPHER,
  CRYPTO_ALG_TYPE_RNG,
};

struct crypto_alg {
  struct list_head list;
  char name[CRYPTO_MAX_ALG_NAME];
  char driver_name[CRYPTO_MAX_ALG_NAME];
  uint32_t priority;
  enum crypto_alg_type type;
  size_t ctx_size;

  int (*init)(void* ctx);
  void (*exit)(void* ctx);

  union {
    struct {
      size_t digestsize;
      size_t blocksize;
      int (*update)(void* ctx, const uint8_t* data, size_t len);
      int (*final)(void* ctx, uint8_t* out);
      int (*digest)(void* ctx, const uint8_t* data, size_t len, uint8_t* out);
    } shash;

    struct {
      size_t min_keysize;
      size_t max_keysize;
      size_t blocksize;
      int (*setkey)(void* ctx, const uint8_t* key, size_t keylen);
      int (*encrypt)(void* ctx, uint8_t* dst, const uint8_t* src);
      int (*decrypt)(void* ctx, uint8_t* dst, const uint8_t* src);
    } cipher;

    struct {
      size_t seedsize;
      int (*generate)(void* ctx, uint8_t* dst, size_t len);
      int (*seed)(void* ctx, const uint8_t* seed, size_t len);
    } rng;
  };
};

/* Registration */
int crypto_register_alg(struct crypto_alg* alg);
int crypto_unregister_alg(struct crypto_alg* alg);

/* High-level API */
struct crypto_tfm {
  struct crypto_alg* alg;
  void* ctx;
};

struct crypto_tfm* crypto_alloc_tfm(const char* name, enum crypto_alg_type type);
void crypto_free_tfm(struct crypto_tfm* tfm);

void* crypto_tfm_ctx(struct crypto_tfm* tfm);

/* SHASH Helpers */
int crypto_shash_update(struct crypto_tfm* tfm, const uint8_t* data, size_t len);
int crypto_shash_final(struct crypto_tfm* tfm, uint8_t* out);
int crypto_shash_digest(struct crypto_tfm* tfm, const uint8_t* data, size_t len, uint8_t* out);
size_t crypto_shash_digestsize(struct crypto_tfm* tfm);
size_t crypto_shash_blocksize(struct crypto_tfm* tfm);

/* Cipher Helpers */
int crypto_cipher_setkey(struct crypto_tfm* tfm, const uint8_t* key, size_t keylen);
int crypto_cipher_encrypt(struct crypto_tfm* tfm, uint8_t* dst, const uint8_t* src);
int crypto_cipher_decrypt(struct crypto_tfm* tfm, uint8_t* dst, const uint8_t* src);

/* RNG Helpers */
int crypto_rng_generate(struct crypto_tfm* tfm, uint8_t* dst, size_t len);
int crypto_rng_seed(struct crypto_tfm* tfm, const uint8_t* seed, size_t len);

/* HMAC */
int crypto_hmac(const char* alg_name, const uint8_t* key, size_t keylen,
                const uint8_t* data, size_t datalen, uint8_t* out);

/* Hardware detection */
bool crypto_has_aes_ni(void);
bool crypto_has_sha_ni(void);
bool crypto_has_rdrand(void);
bool crypto_has_rdseed(void);

int __must_check crypto_init(void);
