#pragma once

#include <kernel/types.h>

uint32_t crc32(const void* data, size_t length);
void crc32_init();