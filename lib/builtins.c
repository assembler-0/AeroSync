/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/builtins.c
 * @brief Compiler builtin implementations
 * @copyright (C) 2025-2026 assembler-0
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

#include <aerosync/types.h>

// IEEE 754 double precision format
typedef union {
  double d;

  struct {
    uint64_t mantissa: 52;
    uint64_t exponent: 11;
    uint64_t sign: 1;
  } parts;
} double_bits;

// IEEE 754 single precision format
typedef union {
  float f;

  struct {
    uint32_t mantissa: 23;
    uint32_t exponent: 8;
    uint32_t sign: 1;
  } parts;
} float_bits;

#define DOUBLE_EXP_MASK 0x7FFULL
#define DOUBLE_MANTISSA_MASK 0xFFFFFFFFFFFFFULL
#define DOUBLE_BIAS 1023

#define FLOAT_EXP_MASK 0xFFU
#define FLOAT_MANTISSA_MASK 0x7FFFFFU
#define FLOAT_BIAS 127

double __adddf3(double a, double b) {
  double_bits ba, bb, result = {0};

  ba.d = a;
  bb.d = b;

  // Handle special cases (zero, infinity, NaN)
  uint64_t exp_a = ba.parts.exponent;
  uint64_t exp_b = bb.parts.exponent;

  // If either operand is NaN, return NaN
  if (exp_a == DOUBLE_EXP_MASK && (ba.parts.mantissa != 0)) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }
  if (exp_b == DOUBLE_EXP_MASK && (bb.parts.mantissa != 0)) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }

  // If one operand is infinity
  if (exp_a == DOUBLE_EXP_MASK) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign;
    return result.d;
  }
  if (exp_b == DOUBLE_EXP_MASK) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 0;
    result.parts.sign = bb.parts.sign;
    return result.d;
  }

  // Extract sign and actual values
  int64_t sign_a = ba.parts.sign ? -1 : 1;
  int64_t sign_b = bb.parts.sign ? -1 : 1;

  // Normalize exponents and mantissas
  uint64_t mant_a = ba.parts.mantissa;
  uint64_t mant_b = bb.parts.mantissa;

  // Handle subnormal numbers
  if (exp_a == 0 && mant_a != 0) {
    // Subnormal - normalize
    while (!(mant_a & (1ULL << 52))) {
      mant_a <<= 1;
      exp_a--;
    }
    exp_a++;
    mant_a &= DOUBLE_MANTISSA_MASK;
  } else if (exp_a != 0) {
    // Normal number - add hidden bit
    mant_a |= (1ULL << 52);
  }

  if (exp_b == 0 && mant_b != 0) {
    // Subnormal - normalize
    while (!(mant_b & (1ULL << 52))) {
      mant_b <<= 1;
      exp_b--;
    }
    exp_b++;
    mant_b &= DOUBLE_MANTISSA_MASK;
  } else if (exp_b != 0) {
    // Normal number - add hidden bit
    mant_b |= (1ULL << 52);
  }

  // Align mantissas based on exponents
  int64_t exp_diff = exp_a - exp_b;
  if (exp_diff > 0) {
    if (exp_diff < 64) {
      mant_b >>= exp_diff;
    } else {
      mant_b = 0; // Too small to matter
    }
  } else if (exp_diff < 0) {
    if (-exp_diff < 64) {
      mant_a >>= -exp_diff;
    } else {
      mant_a = 0; // Too small to matter
    }
    exp_a = exp_b; // Use the larger exponent
  }

  // Perform addition/subtraction
  int64_t result_sign = 0;
  int64_t result_exp = exp_a;
  int64_t result_mant = 0;

  if (sign_a == sign_b) {
    // Same signs - add mantissas
    result_mant = (int64_t) (mant_a + mant_b);
    result_sign = sign_a;
  } else {
    // Different signs - subtract
    if (mant_a >= mant_b) {
      result_mant = (int64_t) (mant_a - mant_b);
      result_sign = sign_a;
    } else {
      result_mant = (int64_t) (mant_b - mant_a);
      result_sign = sign_b;
    }
  }

  // Normalize result
  if (result_mant == 0) {
    result.parts.exponent = 0;
    result.parts.mantissa = 0;
    result.parts.sign = 0; // +0
    return result.d;
  }

  // Normalize the result mantissa
  while (result_mant && !(result_mant & (1ULL << 53))) {
    result_mant <<= 1;
    result_exp--;
  }

  // Check for overflow/underflow
  if (result_exp >= 2047) {
    result.parts.exponent = 2047; // Infinity
    result.parts.mantissa = 0;
    result.parts.sign = (result_sign < 0) ? 1 : 0;
    return result.d;
  }

  if (result_exp <= 0) {
    // Result is subnormal
    if (result_exp < -53) {
      // Too small - return zero
      result.parts.exponent = 0;
      result.parts.mantissa = 0;
      result.parts.sign = (result_sign < 0) ? 1 : 0;
      return result.d;
    }

    // Shift mantissa right to make exponent 0
    result_mant >>= (1 - result_exp);
    result_exp = 0;
  } else {
    // Remove hidden bit
    result_mant &= ~(1ULL << 52);
  }

  result.parts.sign = (result_sign < 0) ? 1 : 0;
  result.parts.exponent = result_exp;
  result.parts.mantissa = result_mant & DOUBLE_MANTISSA_MASK;

  return result.d;
}

double __muldf3(double a, double b) {
  double_bits ba, bb, result = {0};

  ba.d = a;
  bb.d = b;

  // Handle special cases
  uint64_t exp_a = ba.parts.exponent;
  uint64_t exp_b = bb.parts.exponent;

  // If either operand is NaN, return NaN
  if ((exp_a == DOUBLE_EXP_MASK && ba.parts.mantissa != 0) ||
      (exp_b == DOUBLE_EXP_MASK && bb.parts.mantissa != 0)) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }

  // If one operand is infinity and the other is zero, return NaN
  if (((exp_a == DOUBLE_EXP_MASK) && (exp_b == 0 && bb.parts.mantissa == 0)) ||
      ((exp_b == DOUBLE_EXP_MASK) && (exp_a == 0 && ba.parts.mantissa == 0))) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }

  // If either operand is infinity, return infinity
  if (exp_a == DOUBLE_EXP_MASK || exp_b == DOUBLE_EXP_MASK) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign ^ bb.parts.sign;
    return result.d;
  }

  // If either operand is zero, return zero
  if ((exp_a == 0 && ba.parts.mantissa == 0) || (exp_b == 0 && bb.parts.mantissa == 0)) {
    result.parts.exponent = 0;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign ^ bb.parts.sign;
    return result.d;
  }

  // Calculate result sign
  result.parts.sign = ba.parts.sign ^ bb.parts.sign;

  // Extract and normalize mantissas
  uint64_t mant_a = ba.parts.mantissa;
  uint64_t mant_b = bb.parts.mantissa;

  if (exp_a != 0) {
    mant_a |= (1ULL << 52); // Add hidden bit for normal numbers
  }
  if (exp_b != 0) {
    mant_b |= (1ULL << 52);
  }

  // Calculate exponent
  int64_t result_exp = (int64_t) exp_a + (int64_t) exp_b - DOUBLE_BIAS;

  // Multiply mantissas (104-bit result)
  uint64_t hi, lo;
  uint64_t a_hi = mant_a >> 32;
  uint64_t a_lo = mant_a & 0xFFFFFFFFULL;
  uint64_t b_hi = mant_b >> 32;
  uint64_t b_lo = mant_b & 0xFFFFFFFFULL;

  uint64_t p0 = a_lo * b_lo;
  uint64_t p1 = a_lo * b_hi;
  uint64_t p2 = a_hi * b_lo;
  uint64_t p3 = a_hi * b_hi;

  uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);

  lo = (middle << 32) | (p0 & 0xFFFFFFFFULL);
  hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);

  // Normalize the result
  if (hi & (1ULL << 63)) {
    // Result is too big, shift right
    lo = (hi << 1) | (lo >> 63);
    hi >>= 1;
    result_exp++;
  } else if (!(hi & (1ULL << 62))) {
    // Result is too small, shift left
    hi = (hi << 1) | (lo >> 63);
    lo <<= 1;
    result_exp--;
  }

  // Round to nearest even
  uint64_t round_bit = lo & (1ULL << 9); // Bit 10 from LSB (after 52 mantissa bits + 1 normalization bit)
  uint64_t sticky_bits = lo & ((1ULL << 10) - 1); // Bits 0-9

  lo >>= 10; // Shift to get 52 bits of mantissa
  lo &= DOUBLE_MANTISSA_MASK;

  if (round_bit && (sticky_bits || (lo & 1))) {
    lo++; // Round up
    if (lo > DOUBLE_MANTISSA_MASK) {
      lo = 0;
      hi++;
      if (hi & (1ULL << 11)) {
        hi >>= 1;
        result_exp++;
      }
    }
  }

  // Handle overflow in mantissa
  if (hi & (1ULL << 11)) {
    hi >>= 1;
    result_exp++;
  }

  // Check for exponent overflow/underflow
  if (result_exp >= 2047) {
    // Overflow to infinity
    result.parts.exponent = 2047;
    result.parts.mantissa = 0;
    return result.d;
  }

  if (result_exp <= 0) {
    // Subnormal number
    if (result_exp < -52) {
      // Underflow to zero
      result.parts.exponent = 0;
      result.parts.mantissa = 0;
      return result.d;
    }

    // Shift mantissa right and set exponent to 0
    int shift = 1 - result_exp;
    if (shift < 64) {
      uint64_t shifted_lo = lo >> shift;
      uint64_t shifted_hi = hi << (64 - shift);
      lo = shifted_lo | shifted_hi;

      // Check for rounding
      uint64_t round_mask = (1ULL << (shift - 1)) - 1;
      uint64_t round_bits = lo & round_mask;
      lo >>= (shift - 1);

      if ((lo & 1) && (round_bits || (lo & 2))) {
        lo++;
        if (lo > DOUBLE_MANTISSA_MASK) {
          lo = 0;
          result_exp = 1; // Normal number with smallest exponent
        }
      }
    } else {
      // Result is too small for subnormal, return zero
      result.parts.exponent = 0;
      result.parts.mantissa = 0;
      return result.d;
    }

    result.parts.exponent = 0; // Subnormal
  } else {
    // Normal number
    result.parts.exponent = result_exp;
    lo |= (hi & 0x7FFULL) << 52; // Add upper bits of hi to mantissa
  }

  result.parts.mantissa = lo & DOUBLE_MANTISSA_MASK;

  return result.d;
}

double __divdf3(double a, double b) {
  double_bits ba, bb, result = {0};

  ba.d = a;
  bb.d = b;

  // Handle special cases
  uint64_t exp_a = ba.parts.exponent;
  uint64_t exp_b = bb.parts.exponent;

  // If dividend is NaN or divisor is NaN, return NaN
  if ((exp_a == DOUBLE_EXP_MASK && ba.parts.mantissa != 0) ||
      (exp_b == DOUBLE_EXP_MASK && bb.parts.mantissa != 0)) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }

  // Division by zero
  if ((exp_b == 0 && bb.parts.mantissa == 0) &&
      !((exp_a == 0 && ba.parts.mantissa == 0) || (exp_a == DOUBLE_EXP_MASK))) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign ^ bb.parts.sign;
    return result.d;
  }

  // 0/0 or inf/inf -> NaN
  if (((exp_a == 0 && ba.parts.mantissa == 0) && (exp_b == 0 && bb.parts.mantissa == 0)) ||
      ((exp_a == DOUBLE_EXP_MASK) && (exp_b == DOUBLE_EXP_MASK))) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 1;
    result.parts.sign = 0;
    return result.d;
  }

  // Dividend is zero or divisor is infinity
  if ((exp_a == 0 && ba.parts.mantissa == 0) || (exp_b == DOUBLE_EXP_MASK)) {
    result.parts.exponent = 0;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign ^ bb.parts.sign;
    return result.d;
  }

  // Divisor is zero
  if (exp_b == 0 && bb.parts.mantissa == 0) {
    result.parts.exponent = DOUBLE_EXP_MASK;
    result.parts.mantissa = 0;
    result.parts.sign = ba.parts.sign ^ bb.parts.sign;
    return result.d;
  }

  // Calculate result sign
  result.parts.sign = ba.parts.sign ^ bb.parts.sign;

  // Extract and normalize mantissas
  uint64_t mant_a = ba.parts.mantissa;
  uint64_t mant_b = bb.parts.mantissa;

  if (exp_a != 0) {
    mant_a |= (1ULL << 52); // Add hidden bit for normal numbers
  }
  if (exp_b != 0) {
    mant_b |= (1ULL << 52);
  }

  // Calculate exponent
  int64_t result_exp = (int64_t) exp_a - (int64_t) exp_b + DOUBLE_BIAS;

  // Perform division using long division algorithm
  uint64_t quotient = 0;
  uint64_t remainder = mant_a;
  uint64_t divisor = mant_b;

  // Shift remainder to have enough precision
  remainder <<= 12; // Extra precision for rounding
  divisor <<= 1; // Normalize divisor

  // Long division
  for (int i = 0; i < 64; i++) {
    quotient <<= 1;
    remainder <<= 1;

    if (remainder >= divisor) {
      remainder -= divisor;
      quotient |= 1;
    }
  }

  // Normalize the result
  if (quotient & (1ULL << 63)) {
    // Result is normalized
  } else {
    quotient <<= 1;
    result_exp--;
  }

  // Extract mantissa and round
  uint64_t result_mant = (quotient >> 11) & DOUBLE_MANTISSA_MASK;
  uint64_t round_bit = quotient & (1ULL << 10);
  uint64_t sticky_bits = quotient & ((1ULL << 10) - 1);

  if (round_bit && (sticky_bits || (result_mant & 1))) {
    result_mant++;
    if (result_mant > DOUBLE_MANTISSA_MASK) {
      result_mant = 0;
      result_exp++;
    }
  }

  // Check for exponent overflow/underflow
  if (result_exp >= 2047) {
    // Overflow to infinity
    result.parts.exponent = 2047;
    result.parts.mantissa = 0;
    return result.d;
  }

  if (result_exp <= 0) {
    // Subnormal number
    if (result_exp < -52) {
      // Underflow to zero
      result.parts.exponent = 0;
      result.parts.mantissa = 0;
      return result.d;
    }

    result.parts.exponent = 0; // Subnormal
  } else {
    result.parts.exponent = result_exp;
  }

  result.parts.mantissa = result_mant;

  return result.d;
}

float __truncdfsf2(double a) {
  double_bits da;
  float_bits result = {0};

  da.d = a;

  uint64_t exp_d = da.parts.exponent;
  uint64_t mant_d = da.parts.mantissa;

  // Handle special cases
  if (exp_d == DOUBLE_EXP_MASK) {
    // Infinity or NaN
    result.parts.exponent = FLOAT_EXP_MASK;
    result.parts.mantissa = (da.parts.mantissa != 0) ? 1 : 0; // NaN if original was NaN
    result.parts.sign = da.parts.sign;
    return result.f;
  }

  // Extract sign
  result.parts.sign = da.parts.sign;

  // Handle zero
  if (exp_d == 0 && mant_d == 0) {
    result.parts.exponent = 0;
    result.parts.mantissa = 0;
    return result.f;
  }

  // Normalize mantissa for normal numbers
  if (exp_d != 0) {
    mant_d |= (1ULL << 52); // Add hidden bit
  }

  // Convert exponent from double bias to single bias
  int64_t exp_s = (int64_t) exp_d - DOUBLE_BIAS + FLOAT_BIAS;

  // Check for overflow in single precision
  if (exp_s >= FLOAT_EXP_MASK) {
    // Overflow to infinity
    result.parts.exponent = FLOAT_EXP_MASK;
    result.parts.mantissa = 0;
    return result.f;
  }

  // Check for underflow
  if (exp_s <= 0) {
    if (exp_s < -24) {
      // Too small for subnormal - return zero
      result.parts.exponent = 0;
      result.parts.mantissa = 0;
      return result.f;
    }

    // Subnormal number - shift mantissa right
    int shift = 1 - exp_s;
    if (shift < 64) {
      mant_d >>= shift;
    } else {
      mant_d = 0;
    }
    exp_s = 0;
  } else {
    // Normal number - remove hidden bit
    mant_d &= ~(1ULL << 52);
  }

  // Convert 52-bit mantissa to 23-bit mantissa with rounding
  uint64_t round_mask = (1ULL << 29) - 1; // Lower 29 bits for rounding
  uint64_t round_bits = mant_d & round_mask;
  uint32_t result_mant = (mant_d >> 29) & FLOAT_MANTISSA_MASK;

  // Round to nearest, ties to even
  uint64_t round_bit = (mant_d >> 28) & 1; // Bit 28 is the rounding bit
  uint64_t sticky_bits = round_bits || ((mant_d >> 29) & 1); // Any bits after rounding bit

  if (round_bit && (sticky_bits || (result_mant & 1))) {
    result_mant++;
    if (result_mant > FLOAT_MANTISSA_MASK) {
      result_mant = 0;
      exp_s++;
      if (exp_s >= FLOAT_EXP_MASK) {
        // Overflow to infinity
        result.parts.exponent = FLOAT_EXP_MASK;
        result.parts.mantissa = 0;
        return result.f;
      }
    }
  }

  result.parts.exponent = exp_s;
  result.parts.mantissa = result_mant;

  return result.f;
}

double __floatsidf(int a) {
  double_bits result = {0};

  if (a == 0) {
    result.parts.exponent = 0;
    result.parts.mantissa = 0;
    result.parts.sign = 0;
    return result.d;
  }

  // Determine sign and make positive
  result.parts.sign = (a < 0) ? 1 : 0;
  uint32_t ua = (a < 0) ? -a : a;

  // Find the position of the most significant bit
  int exp = 0;
  uint32_t temp = ua;
  while (temp >>= 1) {
    exp++;
  }

  // Calculate the biased exponent
  result.parts.exponent = exp + DOUBLE_BIAS;

  // Shift the mantissa to align with the 52-bit mantissa field
  uint64_t mant = (uint64_t) (ua & ((1U << exp) - 1)); // Get the lower bits after MSB

  if (exp >= 27) {
    // Need to shift right to fit in 52 bits
    int shift = exp - 26; // Leave room for proper alignment
    uint64_t round_bit = (mant >> (shift - 1)) & 1;
    uint64_t sticky_bits = (mant & ((1ULL << (shift - 1)) - 1)) != 0;
    mant >>= shift;

    // Round to nearest, ties to even
    if (round_bit && (sticky_bits || (mant & 1))) {
      mant++;
      if (mant > DOUBLE_MANTISSA_MASK) {
        mant = 0;
        result.parts.exponent++;
      }
    }
  } else if (exp > 0) {
    // Shift left to fill the 52-bit mantissa space
    mant <<= (52 - exp);
  }

  result.parts.mantissa = mant & DOUBLE_MANTISSA_MASK;

  return result.d;
}

__uint128_t __udivti3(__uint128_t a, __uint128_t b) {
  __uint128_t q = 0;
  for (int i = 127; i >= 0; i--) {
    if ((b << i) <= a) {
      a -= b << i;
      q |= ((__uint128_t) 1 << i);
    }
  }
  return q;
}

#include <aerosync/export.h>

EXPORT_SYMBOL(__adddf3);
EXPORT_SYMBOL(__muldf3);
EXPORT_SYMBOL(__divdf3);
EXPORT_SYMBOL(__truncdfsf2);
EXPORT_SYMBOL(__floatsidf);
