/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/hmac.c
 * @brief Generic HMAC implementation using Crypto API
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <aerosync/errno.h>
#include <mm/slub.h>
#include <lib/string.h>

int crypto_hmac(const char *alg_name, const uint8_t *key, size_t keylen,
                const uint8_t *data, size_t datalen, uint8_t *out) {
  struct crypto_tfm *tfm = crypto_alloc_tfm(alg_name, CRYPTO_ALG_TYPE_SHASH);
  if (!tfm) return -EINVAL;
    
  size_t digestsize = crypto_shash_digestsize(tfm);
  size_t blocksize = crypto_shash_blocksize(tfm);
    
  uint8_t *ipad = kmalloc(blocksize);
  uint8_t *opad = kmalloc(blocksize);
  uint8_t *key_buf = kmalloc(blocksize);
    
  if (!ipad || !opad || !key_buf) {
    kfree(ipad); kfree(opad); kfree(key_buf);
    crypto_free_tfm(tfm);
    return -ENOMEM;
  }
    
  memset(key_buf, 0, blocksize);
  if (keylen > blocksize) {
    crypto_shash_digest(tfm, key, keylen, key_buf);
    keylen = digestsize;
  } else {
    memcpy(key_buf, key, keylen);
  }
    
  for (size_t i = 0; i < blocksize; i++) {
    ipad[i] = key_buf[i] ^ 0x36;
    opad[i] = key_buf[i] ^ 0x5c;
  }
    
  uint8_t *inner_hash = kmalloc(digestsize);
  if (!inner_hash) {
    kfree(ipad); kfree(opad); kfree(key_buf);
    crypto_free_tfm(tfm);
    return -ENOMEM;
  }
    
  /* Reset TFM state for inner hash */
  tfm->alg->init(tfm->ctx);
  crypto_shash_update(tfm, ipad, blocksize);
  crypto_shash_update(tfm, data, datalen);
  crypto_shash_final(tfm, inner_hash);
    
  /* Reset TFM state for outer hash */
  tfm->alg->init(tfm->ctx);
  crypto_shash_update(tfm, opad, blocksize);
  crypto_shash_update(tfm, inner_hash, digestsize);
  crypto_shash_final(tfm, out);
    
  kfree(inner_hash);
  kfree(ipad); kfree(opad); kfree(key_buf);
  crypto_free_tfm(tfm);
    
  return 0;
}
