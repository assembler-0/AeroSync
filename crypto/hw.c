/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file crypto/hw.c
 * @brief Hardware-accelerated crypto detection and low-level helpers
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <arch/x86_64/cpu.h>

bool crypto_has_aes_ni(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, &eax, &ebx, &ecx, &edx);
  return (ecx & (1u << 25)) != 0;
}

bool crypto_has_sha_ni(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
  return (ebx & (1u << 29)) != 0;
}

bool crypto_has_rdrand(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, &eax, &ebx, &ecx, &edx);
  return (ecx & (1u << 30)) != 0;
}

bool crypto_has_rdseed(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
  return (ebx & (1u << 18)) != 0;
}

/* Low-level helpers for kernel subsystems (e.g. KASLR) */

int rdrand_supported(void) {
  return crypto_has_rdrand();
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

int rdrand16_safe(uint16_t *rand) {
  unsigned char ok;
  __asm__ volatile ("rdrand %0; setc %1" : "=r" (*rand), "=qm" (ok));
  return ok;
}

int rdrand32_safe(uint32_t *rand) {
  unsigned char ok;
  __asm__ volatile ("rdrand %0; setc %1" : "=r" (*rand), "=qm" (ok));
  return ok;
}

int rdrand64_safe(uint64_t *rand) {
  unsigned char ok;
  __asm__ volatile ("rdrand %0; setc %1" : "=r" (*rand), "=qm" (ok));
  return ok;
}
