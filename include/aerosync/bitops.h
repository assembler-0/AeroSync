#pragma once

#include <arch/x86_64/bitops.h>
#include <compiler.h>

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
#define BITS_TO_LONGS(x) \
  (((x) + 8 * sizeof (unsigned long) - 1) / (8 * sizeof (unsigned long)))


static __always_inline __attribute_const__ unsigned long variable__ffs(unsigned long word)
{
	__asm__ volatile("tzcnt %1,%0"
		: "=r" (word)
		: ASM_INPUT_RM (word));
	return word;
}

/**
 * __ffs - find first set bit in word
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
#define __ffs(word)				\
	(__builtin_constant_p(word) ?		\
	 (unsigned long)__builtin_ctzl(word) :	\
	 variable__ffs(word))

static __always_inline __attribute_const__ unsigned long variable_ffz(unsigned long word)
{
	return variable__ffs(~word);
}

/**
 * ffz - find first zero bit in word
 * @word: The word to search
 *
 * Undefined if no zero exists, so code should check against ~0UL first.
 */
#define ffz(word)				\
	(__builtin_constant_p(word) ?		\
	 (unsigned long)__builtin_ctzl(~word) :	\
	 variable_ffz(word))

/*
 * __fls: find last set bit in word
 * @word: The word to search
 *
 * Undefined if no set bit exists, so code should check against 0 first.
 */
static __always_inline __attribute_const__ unsigned long __fls(unsigned long word)
{
	if (__builtin_constant_p(word))
		return BITS_PER_LONG - 1 - __builtin_clzl(word);

	__asm__ volatile("bsr %1,%0"
	    : "=r" (word)
	    : ASM_INPUT_RM (word));
	return word;
}

#undef ADDR

static __always_inline __attribute_const__ int variable_ffs(int x)
{
	int r;

	/*
	 * AMD64 says BSFL won't clobber the dest reg if x==0; Intel64 says the
	 * dest reg is undefined if x==0, but their CPU architect says its
	 * value is written to set it to the same as before, except that the
	 * top 32 bits will be cleared.
	 *
	 * We cannot do this on 32 bits because at the very least some
	 * 486 CPUs did not behave this way.
	 */
	__asm__ volatile("bsfl %1,%0"
	    : "=r" (r)
	    : ASM_INPUT_RM (x), "0" (-1));
	return r + 1;
}

/**
 * ffs - find first set bit in word
 * @x: the word to search
 *
 * This is defined the same way as the libc and compiler builtin ffs
 * routines, therefore differs in spirit from the other bitops.
 *
 * ffs(value) returns 0 if value is 0 or the position of the first
 * set bit if value is nonzero. The first (least significant) bit
 * is at position 1.
 */
#define ffs(x) (__builtin_constant_p(x) ? __builtin_ffs(x) : variable_ffs(x))

/**
 * fls - find last set bit in word
 * @x: the word to search
 *
 * This is defined in a similar way as the libc and compiler builtin
 * ffs, but returns the position of the most significant set bit.
 *
 * fls(value) returns 0 if value is 0 or the position of the last
 * set bit if value is nonzero. The last (most significant) bit is
 * at position 32.
 */
static __always_inline __attribute_const__ int fls(unsigned int x)
{
	int r;

	if (__builtin_constant_p(x))
		return x ? 32 - __builtin_clz(x) : 0;

	__asm__ volatile("bsrl %1,%0"
	    : "=r" (r)
	    : ASM_INPUT_RM (x), "0" (-1));
	return r + 1;
}

/**
 * fls64 - find last set bit in a 64-bit word
 * @x: the word to search
 *
 * This is defined in a similar way as the libc and compiler builtin
 * ffsll, but returns the position of the most significant set bit.
 *
 * fls64(value) returns 0 if value is 0 or the position of the last
 * set bit if value is nonzero. The last (most significant) bit is
 * at position 64.
 */
static __always_inline __attribute_const__ int fls64(uint64_t x)
{
	int bitpos = -1;

	if (__builtin_constant_p(x))
		return x ? 64 - __builtin_clzll(x) : 0;
	/*
	 * AMD64 says BSRQ won't clobber the dest reg if x==0; Intel64 says the
	 * dest reg is undefined if x==0, but their CPU architect says its
	 * value is written to set it to the same as before.
	 */
	__asm__ volatile("bsrq %1,%q0"
	    : "+r" (bitpos)
	    : ASM_INPUT_RM (x));
	return bitpos + 1;
}

static inline unsigned int fls_long(unsigned long l)
{
  if (sizeof(l) == 4)
    return fls(l);
  return fls64(l);
}