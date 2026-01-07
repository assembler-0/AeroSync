/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/rng.c
 * @brief Random number generator implementations
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <crypto/rng.h>
#include <arch/x86_64/cpu.h>

static uint64_t s[2]; // state

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t xoroshiro128plus(void) {
    uint64_t s0 = s[0];
    uint64_t s1 = s[1];
    uint64_t result = s0 + s1;
    s1 ^= s0;
    s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14);
    s[1] = rotl(s1, 36);
    return result;
}

static uint64_t splitmix64(uint64_t* x) {
    *x += 0x9e3779b97f4a7c15ULL;
    uint64_t z = *x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void rng_seed(uint64_t seed_lo, uint64_t seed_hi) {
    uint64_t seed = seed_lo ^ (seed_hi + 0x9e3779b97f4a7c15ULL);
    s[0] ^= splitmix64(&seed);
    s[1] ^= splitmix64(&seed);
    if ((s[0] | s[1]) == 0) s[0] = 0x8a5cd789635d2dffULL; // ensure non-zero state
}

int rdrand_supported(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 30)) != 0;
}

uint16_t rdrand16(void) {
    uint16_t r;
    __asm__ volatile("rdrand %0" : "=r"(r));
    return r;
}

uint32_t rdrand32(void) {
    uint32_t r;
    __asm__ volatile("rdrand %0" : "=r"(r));
    return r;
}

uint64_t rdrand64(void) {
    uint64_t r;
    __asm__ volatile("rdrand %0" : "=r"(r));
    return r;
}
