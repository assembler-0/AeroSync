/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MATH64_H
#define _LINUX_MATH64_H

#include <aerosync/types.h>
#include <compiler.h>
#include <lib/math.h>

/**
 * do_div - returns 2 values: calculate remainder and update new dividend
 * @n: uint64_t dividend (will be updated)
 * @base: uint32_t divisor
 *
 * Summary:
 * ``uint32_t remainder = n % base;``
 * ``n = n / base;``
 *
 * Return: (uint32_t)remainder
 *
 * NOTE: macro parameter @n is evaluated multiple times,
 * beware of side effects!
 */
# define do_div(n,base) ({					\
uint32_t __base = (base);				\
uint32_t __rem;						\
__rem = ((uint64_t)(n)) % __base;			\
(n) = ((uint64_t)(n)) / __base;				\
__rem;							\
})

#define div64_long(x, y) div64_s64((x), (y))
#define div64_ul(x, y)   div64_u64((x), (y))

/**
 * div_u64_rem - unsigned 64bit divide with 32bit divisor with remainder
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 32bit divisor
 * @remainder: pointer to unsigned 32bit remainder
 *
 * Return: sets ``*remainder``, then returns dividend / divisor
 *
 * This is commonly provided by 32bit archs to provide an optimized 64bit
 * divide.
 */
static inline uint64_t div_u64_rem(uint64_t dividend, uint32_t divisor, uint32_t *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

/**
 * div_s64_rem - signed 64bit divide with 32bit divisor with remainder
 * @dividend: signed 64bit dividend
 * @divisor: signed 32bit divisor
 * @remainder: pointer to signed 32bit remainder
 *
 * Return: sets ``*remainder``, then returns dividend / divisor
 */
static inline int64_t div_s64_rem(int64_t dividend, int32_t divisor, int32_t *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

/**
 * div64_u64_rem - unsigned 64bit divide with 64bit divisor and remainder
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 64bit divisor
 * @remainder: pointer to unsigned 64bit remainder
 *
 * Return: sets ``*remainder``, then returns dividend / divisor
 */
static inline uint64_t div64_u64_rem(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

/**
 * div64_u64 - unsigned 64bit divide with 64bit divisor
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 64bit divisor
 *
 * Return: dividend / divisor
 */
static inline uint64_t div64_u64(uint64_t dividend, uint64_t divisor)
{
	return dividend / divisor;
}

/**
 * div64_s64 - signed 64bit divide with 64bit divisor
 * @dividend: signed 64bit dividend
 * @divisor: signed 64bit divisor
 *
 * Return: dividend / divisor
 */
static inline int64_t div64_s64(int64_t dividend, int64_t divisor)
{
	return dividend / divisor;
}

/**
 * div_u64 - unsigned 64bit divide with 32bit divisor
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 32bit divisor
 *
 * This is the most common 64bit divide and should be used if possible,
 * as many 32bit archs can optimize this variant better than a full 64bit
 * divide.
 *
 * Return: dividend / divisor
 */
#ifndef div_u64
static inline uint64_t div_u64(uint64_t dividend, uint32_t divisor)
{
	uint32_t remainder;
	return div_u64_rem(dividend, divisor, &remainder);
}
#endif

/**
 * div_s64 - signed 64bit divide with 32bit divisor
 * @dividend: signed 64bit dividend
 * @divisor: signed 32bit divisor
 *
 * Return: dividend / divisor
 */
#ifndef div_s64
static inline int64_t div_s64(int64_t dividend, int32_t divisor)
{
	int32_t remainder;
	return div_s64_rem(dividend, divisor, &remainder);
}
#endif

uint32_t iter_div_u64_rem(uint64_t dividend, uint32_t divisor, uint64_t *remainder);

#ifndef mul_u32_u32
/*
 * Many a GCC version messes this up and generates a 64x64 mult :-(
 */
static inline uint64_t mul_u32_u32(uint32_t a, uint32_t b)
{
	return (uint64_t)a * b;
}
#endif

#ifndef add_u64_u32
/*
 * Many a GCC version also messes this up.
 * Zero extending b and then spilling everything to stack.
 */
static inline uint64_t add_u64_u32(uint64_t a, uint32_t b)
{
	return a + b;
}
#endif

#if defined(CONFIG_ARCH_SUPPORTS_INT128) && defined(__SIZEOF_INT128__)

#ifndef mul_u64_u32_shr
static __always_inline uint64_t mul_u64_u32_shr(uint64_t a, uint32_t mul, unsigned int shift)
{
	return (uint64_t)(((unsigned __int128)a * mul) >> shift);
}
#endif /* mul_u64_u32_shr */

#ifndef mul_u64_u64_shr
static __always_inline uint64_t mul_u64_u64_shr(uint64_t a, uint64_t mul, unsigned int shift)
{
	return (uint64_t)(((unsigned __int128)a * mul) >> shift);
}
#endif /* mul_u64_u64_shr */

#else

#ifndef mul_u64_u32_shr
static __always_inline uint64_t mul_u64_u32_shr(uint64_t a, uint32_t mul, unsigned int shift)
{
	uint32_t ah = a >> 32, al = a;
	uint64_t ret;

	ret = mul_u32_u32(al, mul) >> shift;
	if (ah)
		ret += mul_u32_u32(ah, mul) << (32 - shift);
	return ret;
}
#endif /* mul_u64_u32_shr */

#ifndef mul_u64_u64_shr
static inline uint64_t mul_u64_u64_shr(uint64_t a, uint64_t b, unsigned int shift)
{
	union {
		uint64_t ll;
		struct {
#ifdef __BIG_ENDIAN
			uint32_t high, low;
#else
			uint32_t low, high;
#endif
		} l;
	} rl, rm, rn, rh, a0, b0;
	uint64_t c;

	a0.ll = a;
	b0.ll = b;

	rl.ll = mul_u32_u32(a0.l.low, b0.l.low);
	rm.ll = mul_u32_u32(a0.l.low, b0.l.high);
	rn.ll = mul_u32_u32(a0.l.high, b0.l.low);
	rh.ll = mul_u32_u32(a0.l.high, b0.l.high);

	/*
	 * Each of these lines computes a 64-bit intermediate result into "c",
	 * starting at bits 32-95.  The low 32-bits go into the result of the
	 * multiplication, the high 32-bits are carried into the next step.
	 */
	rl.l.high = c = (uint64_t)rl.l.high + rm.l.low + rn.l.low;
	rh.l.low = c = (c >> 32) + rm.l.high + rn.l.high + rh.l.low;
	rh.l.high = (c >> 32) + rh.l.high;

	/*
	 * The 128-bit result of the multiplication is in rl.ll and rh.ll,
	 * shift it right and throw away the high part of the result.
	 */
	if (shift == 0)
		return rl.ll;
	if (shift < 64)
		return (rl.ll >> shift) | (rh.ll << (64 - shift));
	return rh.ll >> (shift & 63);
}
#endif /* mul_u64_u64_shr */

#endif

#ifndef mul_s64_u64_shr
static inline uint64_t mul_s64_u64_shr(int64_t a, uint64_t b, unsigned int shift)
{
	uint64_t ret;

	/*
	 * Extract the sign before the multiplication and put it back
	 * afterwards if needed.
	 */
	ret = mul_u64_u64_shr(abs(a), b, shift);

	if (a < 0)
		ret = -((int64_t) ret);

	return ret;
}
#endif /* mul_s64_u64_shr */

#ifndef mul_u64_u32_div
static inline uint64_t mul_u64_u32_div(uint64_t a, uint32_t mul, uint32_t divisor)
{
	union {
		uint64_t ll;
		struct {
#ifdef __BIG_ENDIAN
			uint32_t high, low;
#else
			uint32_t low, high;
#endif
		} l;
	} u, rl, rh;

	u.ll = a;
	rl.ll = mul_u32_u32(u.l.low, mul);
	rh.ll = mul_u32_u32(u.l.high, mul) + rl.l.high;

	/* Bits 32-63 of the result will be in rh.l.low. */
	rl.l.high = do_div(rh.ll, divisor);

	/* Bits 0-31 of the result will be in rl.l.low.	*/
	do_div(rl.ll, divisor);

	rl.l.high = rh.l.low;
	return rl.ll;
}
#endif /* mul_u64_u32_div */

/**
 * mul_u64_add_u64_div_u64 - unsigned 64bit multiply, add, and divide
 * @a: first unsigned 64bit multiplicand
 * @b: second unsigned 64bit multiplicand
 * @c: unsigned 64bit addend
 * @d: unsigned 64bit divisor
 *
 * Multiply two 64bit values together to generate a 128bit product
 * add a third value and then divide by a fourth.
 * The Generic code divides by 0 if @d is zero and returns ~0 on overflow.
 * Architecture specific code may trap on zero or overflow.
 *
 * Return: (@a * @b + @c) / @d
 */
uint64_t mul_u64_add_u64_div_u64(uint64_t a, uint64_t b, uint64_t c, uint64_t d);

/**
 * mul_u64_u64_div_u64 - unsigned 64bit multiply and divide
 * @a: first unsigned 64bit multiplicand
 * @b: second unsigned 64bit multiplicand
 * @d: unsigned 64bit divisor
 *
 * Multiply two 64bit values together to generate a 128bit product
 * and then divide by a third value.
 * The Generic code divides by 0 if @d is zero and returns ~0 on overflow.
 * Architecture specific code may trap on zero or overflow.
 *
 * Return: @a * @b / @d
 */
#define mul_u64_u64_div_u64(a, b, d) mul_u64_add_u64_div_u64(a, b, 0, d)

/**
 * mul_u64_u64_div_u64_roundup - unsigned 64bit multiply and divide rounded up
 * @a: first unsigned 64bit multiplicand
 * @b: second unsigned 64bit multiplicand
 * @d: unsigned 64bit divisor
 *
 * Multiply two 64bit values together to generate a 128bit product
 * and then divide and round up.
 * The Generic code divides by 0 if @d is zero and returns ~0 on overflow.
 * Architecture specific code may trap on zero or overflow.
 *
 * Return: (@a * @b + @d - 1) / @d
 */
#define mul_u64_u64_div_u64_roundup(a, b, d) \
	({ uint64_t _tmp = (d); mul_u64_add_u64_div_u64(a, b, _tmp - 1, _tmp); })


/**
 * DIV64_U64_ROUND_UP - unsigned 64bit divide with 64bit divisor rounded up
 * @ll: unsigned 64bit dividend
 * @d: unsigned 64bit divisor
 *
 * Divide unsigned 64bit dividend by unsigned 64bit divisor
 * and round up.
 *
 * Return: dividend / divisor rounded up
 */
#define DIV64_U64_ROUND_UP(ll, d)	\
	({ uint64_t _tmp = (d); div64_u64((ll) + _tmp - 1, _tmp); })

/**
 * DIV_U64_ROUND_UP - unsigned 64bit divide with 32bit divisor rounded up
 * @ll: unsigned 64bit dividend
 * @d: unsigned 32bit divisor
 *
 * Divide unsigned 64bit dividend by unsigned 32bit divisor
 * and round up.
 *
 * Return: dividend / divisor rounded up
 */
#define DIV_U64_ROUND_UP(ll, d)		\
	({ uint32_t _tmp = (d); div_u64((ll) + _tmp - 1, _tmp); })

/**
 * DIV64_U64_ROUND_CLOSEST - unsigned 64bit divide with 64bit divisor rounded to nearest integer
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 64bit divisor
 *
 * Divide unsigned 64bit dividend by unsigned 64bit divisor
 * and round to closest integer.
 *
 * Return: dividend / divisor rounded to nearest integer
 */
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor)	\
	({ uint64_t _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })

/**
 * DIV_U64_ROUND_CLOSEST - unsigned 64bit divide with 32bit divisor rounded to nearest integer
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 32bit divisor
 *
 * Divide unsigned 64bit dividend by unsigned 32bit divisor
 * and round to closest integer.
 *
 * Return: dividend / divisor rounded to nearest integer
 */
#define DIV_U64_ROUND_CLOSEST(dividend, divisor)	\
	({ uint32_t _tmp = (divisor); div_u64((uint64_t)(dividend) + _tmp / 2, _tmp); })

/**
 * DIV_S64_ROUND_CLOSEST - signed 64bit divide with 32bit divisor rounded to nearest integer
 * @dividend: signed 64bit dividend
 * @divisor: signed 32bit divisor
 *
 * Divide signed 64bit dividend by signed 32bit divisor
 * and round to closest integer.
 *
 * Return: dividend / divisor rounded to nearest integer
 */
#define DIV_S64_ROUND_CLOSEST(dividend, divisor)(	\
{							\
	int64_t __x = (dividend);				\
	int32_t __d = (divisor);				\
	((__x > 0) == (__d > 0)) ?			\
		div_s64((__x + (__d / 2)), __d) :	\
		div_s64((__x - (__d / 2)), __d);	\
}							\
)

/**
 * roundup_u64 - Round up a 64bit value to the next specified 32bit multiple
 * @x: the value to up
 * @y: 32bit multiple to round up to
 *
 * Rounds @x to the next multiple of @y. For 32bit @x values, see roundup and
 * the faster round_up() for powers of 2.
 *
 * Return: rounded up value.
 */
static inline uint64_t roundup_u64(uint64_t x, uint32_t y)
{
	return DIV_U64_ROUND_UP(x, y) * y;
}
#endif /* _LINUX_MATH64_H */