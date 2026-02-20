// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/uuid.c
 * @brief UUID/GUID implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <linux/uuid.h>
#include <aerosync/crypto.h>
#include <lib/string.h>
#include <aerosync/errno.h>
#include <aerosync/ctype.h>

bool uuid_is_null(const uuid_t *uuid) {
  for (int i = 0; i < 16; i++) {
    if (uuid->b[i] != 0) return false;
  }
  return true;
}

void uuid_gen(uuid_t *uuid) {
  struct crypto_tfm *tfm = crypto_alloc_tfm("sw_rng", CRYPTO_ALG_TYPE_RNG);
  if (tfm) {
    crypto_rng_generate(tfm, uuid->b, 16);
    crypto_free_tfm(tfm);
  } else {
    /* Fallback if RNG is not available (should not happen in full system) */
    memset(uuid->b, 0, 16);
  }

  /* UUID v4: bits 6-7 of byte 8 are 10, bits 4-7 of byte 6 are 0100 */
  uuid->b[6] = (uuid->b[6] & 0x0f) | 0x40;
  uuid->b[8] = (uuid->b[8] & 0x3f) | 0x80;
}

void guid_gen(guid_t *guid) {
  /* GUID is often just a UUID in little-endian for the first 3 fields */
  /* For our purpose, we treat them as 16 random bytes for now */
  uuid_gen((uuid_t *)guid);
}

static int hex_to_bin(char ch) {
  if (isdigit(ch)) return ch - '0';
  ch = tolower(ch);
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

int uuid_parse(const char *in, uuid_t *uuid) {
  if (strlen(in) < 36) return -EINVAL;

  int i, j;
  for (i = 0, j = 0; i < 36; j++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (in[i] != '-') return -EINVAL;
      i++;
    }

    int high = hex_to_bin(in[i++]);
    int low = hex_to_bin(in[i++]);
    if (high < 0 || low < 0) return -EINVAL;

    uuid->b[j] = (high << 4) | low;
  }

  return 0;
}

void uuid_to_string(const uuid_t *uuid, char *out) {
  static const char hex[] = "0123456789abcdef";
  int i, j;
  for (i = 0, j = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[j++] = '-';
    }
    out[j++] = hex[uuid->b[i] >> 4];
    out[j++] = hex[uuid->b[i] & 0x0f];
  }
  out[j] = '\0';
}
