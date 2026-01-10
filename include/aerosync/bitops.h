#pragma once

#include <arch/x86_64/bitops.h>

/**
 * @file include/aerosync/bitops.h
 * @brief Generic bit operations interface
 */

/*
 * Find-bit operations (generic or arch-optimized)
 */
#define BIT(nr) (1UL << (nr))
#define BIT_MASK(nr) (1UL << ((nr) % 64))
#define BIT_WORD(nr) ((nr) / 64)
