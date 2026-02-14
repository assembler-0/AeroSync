/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/init.c
 * @brief Cryptography subsystem initialization
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <aerosync/classes.h>
#include <lib/printk.h>

/* Forward declarations of internal init functions */
int sha256_generic_init(void);
int sha256_ni_init(void);
int sha512_generic_init(void);
int sha1_generic_init(void);
int blake2s_generic_init(void);
int crc32_generic_init(void);
int aes_generic_init(void);
int aes_ni_init(void);
int hw_rng_init(void);
int sw_rng_init(void);
int crypto_sysintf_init(void);

void crypto_init(void) {
  sha256_generic_init();
  sha256_ni_init();
  sha512_generic_init();
  sha1_generic_init();
  blake2s_generic_init();
  crc32_generic_init();
  aes_generic_init();
  aes_ni_init();
  sw_rng_init();
  hw_rng_init();
  crypto_sysintf_init();
    
  printk(KERN_INFO CRYPTO_CLASS "initialized (aes-ni: %s, sha-ni: %s, rdrand: %s)\n",
         crypto_has_aes_ni() ? "yes" : "no",
         crypto_has_sha_ni() ? "yes" : "no",
         crypto_has_rdrand() ? "yes" : "no");
}
