/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include <aerosync/types.h>

typedef struct {
  u8 b[16];
} uuid_t;

typedef struct {
  u8 b[16];
} guid_t;

#define UUID_SIZE 16
#define GUID_SIZE 16

bool uuid_is_null(const uuid_t *uuid);
void uuid_gen(uuid_t *uuid);
void guid_gen(guid_t *guid);

/* UUID-v4 generation (random) */
int uuid_parse(const char *in, uuid_t *uuid);
void uuid_to_string(const uuid_t *uuid, char *out);
