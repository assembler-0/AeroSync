/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SWAB_H
#define _UAPI_LINUX_SWAB_H

#include <asm/swab.h>
#include <aerosync/compiler.h>

/*
 * Implement the following as inlines, but define the interface using
 * macros to allow constant folding when possible:
 * ___swab16, ___swab32, ___swab64, ___swahw32, ___swahb32
 */

static inline __attribute_const__ uint16_t __fswab16(uint16_t val)
{
#if defined (__arch_swab16)
	return __arch_swab16(val);
#else
	return ___constant_swab16(val);
#endif
}

static inline __attribute_const__ uint32_t __fswab32(uint32_t val)
{
#if defined(__arch_swab32)
	return __arch_swab32(val);
#else
	return ___constant_swab32(val);
#endif
}

static inline __attribute_const__ uint64_t __fswab64(uint64_t val)
{
#if defined (__arch_swab64)
	return __arch_swab64(val);
#elif defined(__SWAB_64_THRU_32__)
	uint32_t h = val >> 32;
	uint32_t l = val & ((1ULL << 32) - 1);
	return (((uint64_t)__fswab32(l)) << 32) | ((uint64_t)(__fswab32(h)));
#else
	return ___constant_swab64(val);
#endif
}

/**
 * __swab16 - return a byteswapped 16-bit value
 * @x: value to byteswap
 */
#ifdef __HAVE_BUILTIN_BSWAP16__
#define __swab16(x) (uint16_t)__builtin_bswap16((uint16_t)(x))
#else
#define __swab16(x)				\
	(uint16_t)(__builtin_constant_p(x) ?	\
	___constant_swab16(x) :			\
	__fswab16(x))
#endif

/**
 * __swab32 - return a byteswapped 32-bit value
 * @x: value to byteswap
 */
#ifdef __HAVE_BUILTIN_BSWAP32__
#define __swab32(x) (uint32_t)__builtin_bswap32((uint32_t)(x))
#else
#define __swab32(x)				\
	(uint32_t)(__builtin_constant_p(x) ?	\
	___constant_swab32(x) :			\
	__fswab32(x))
#endif

/**
 * __swab64 - return a byteswapped 64-bit value
 * @x: value to byteswap
 */
#ifdef __HAVE_BUILTIN_BSWAP64__
#define __swab64(x) (uint64_t)__builtin_bswap64((uint64_t)(x))
#else
#define __swab64(x)				\
	(uint64_t)(__builtin_constant_p(x) ?	\
	___constant_swab64(x) :			\
	__fswab64(x))
#endif

static __always_inline unsigned long __swab(const unsigned long y)
{
	return __swab64(y);
}

/**
 * __swahw32 - return a word-swapped 32-bit value
 * @x: value to wordswap
 *
 * __swahw32(0x12340000) is 0x00001234
 */
#define __swahw32(x)				\
	(__builtin_constant_p((uint32_t)(x)) ?	\
	___constant_swahw32(x) :		\
	__fswahw32(x))

/**
 * __swahb32 - return a high and low byte-swapped 32-bit value
 * @x: value to byteswap
 *
 * __swahb32(0x12345678) is 0x34127856
 */
#define __swahb32(x)				\
	(__builtin_constant_p((uint32_t)(x)) ?	\
	___constant_swahb32(x) :		\
	__fswahb32(x))

/**
 * __swab16p - return a byteswapped 16-bit value from a pointer
 * @p: pointer to a naturally-aligned 16-bit value
 */
static __always_inline uint16_t __swab16p(const uint16_t *p)
{
#ifdef __arch_swab16p
	return __arch_swab16p(p);
#else
	return __swab16(*p);
#endif
}

/**
 * __swab32p - return a byteswapped 32-bit value from a pointer
 * @p: pointer to a naturally-aligned 32-bit value
 */
static __always_inline uint32_t __swab32p(const uint32_t *p)
{
#ifdef __arch_swab32p
	return __arch_swab32p(p);
#else
	return __swab32(*p);
#endif
}

/**
 * __swab64p - return a byteswapped 64-bit value from a pointer
 * @p: pointer to a naturally-aligned 64-bit value
 */
static __always_inline uint64_t __swab64p(const uint64_t *p)
{
#ifdef __arch_swab64p
	return __arch_swab64p(p);
#else
	return __swab64(*p);
#endif
}

/**
 * __swab16s - byteswap a 16-bit value in-place
 * @p: pointer to a naturally-aligned 16-bit value
 */
static inline void __swab16s(uint16_t *p)
{
#ifdef __arch_swab16s
	__arch_swab16s(p);
#else
	*p = __swab16p(p);
#endif
}
/**
 * __swab32s - byteswap a 32-bit value in-place
 * @p: pointer to a naturally-aligned 32-bit value
 */
static __always_inline void __swab32s(uint32_t *p)
{
#ifdef __arch_swab32s
	__arch_swab32s(p);
#else
	*p = __swab32p(p);
#endif
}

/**
 * __swab64s - byteswap a 64-bit value in-place
 * @p: pointer to a naturally-aligned 64-bit value
 */
static __always_inline void __swab64s(uint64_t *p)
{
#ifdef __arch_swab64s
	__arch_swab64s(p);
#else
	*p = __swab64p(p);
#endif
}

#endif /* _UAPI_LINUX_SWAB_H */