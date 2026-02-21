/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_UNALIGNED_H
#define __LINUX_UNALIGNED_H

/*
 * This is the most generic implementation of unaligned accesses
 * and should work almost anywhere.
 */

#include <aerosync/types.h>
#include <aerosync/compiler.h>
#include <asm/byteorder.h>
#include <linux/types.h>

/**
 * __get_unaligned_t - read an unaligned value from memory.
 * @type:	the type to load from the pointer.
 * @ptr:	the pointer to load from.
 *
 * Use memcpy to affect an unaligned type sized load avoiding undefined behavior
 * from approaches like type punning that require -fno-strict-aliasing in order
 * to be correct. As type may be const, use __unqual_scalar_typeof to map to a
 * non-const type - you can't memcpy into a const type. The
 * __get_unaligned_ctrl_type gives __unqual_scalar_typeof its required
 * expression rather than type, a pointer is used to avoid warnings about mixing
 * the use of 0 and NULL. The void* cast silences ubsan warnings.
 */
#define __get_unaligned_t(type, ptr) ({					\
	type *__get_unaligned_ctrl_type __unused = nullptr;		\
	__unqual_scalar_typeof(*__get_unaligned_ctrl_type) __get_unaligned_val; \
	__builtin_memcpy(&__get_unaligned_val, (void *)(ptr),		\
			 sizeof(__get_unaligned_val));			\
	__get_unaligned_val;						\
})

/**
 * __put_unaligned_t - write an unaligned value to memory.
 * @type:	the type of the value to store.
 * @val:	the value to store.
 * @ptr:	the pointer to store to.
 *
 * Use memcpy to affect an unaligned type sized store avoiding undefined
 * behavior from approaches like type punning that require -fno-strict-aliasing
 * in order to be correct. The void* cast silences ubsan warnings.
 */
#define __put_unaligned_t(type, val, ptr) do {				\
	type __put_unaligned_val = (val);				\
	__builtin_memcpy((void *)(ptr), &__put_unaligned_val,		\
			 sizeof(__put_unaligned_val));			\
} while (0)


#define get_unaligned(ptr)	__get_unaligned_t(typeof(*(ptr)), (ptr))
#define put_unaligned(val, ptr) __put_unaligned_t(typeof(*(ptr)), (val), (ptr))

struct __una_u16 { uint16_t x; } __packed;
struct __una_u32 { uint32_t x; } __packed;
struct __una_u64 { uint64_t x; } __packed;

static inline uint16_t __get_unaligned_cpu16(const void *p)
{
  const struct __una_u16 *ptr = (const struct __una_u16 *)p;
  return ptr->x;
}

static inline uint32_t __get_unaligned_cpu32(const void *p)
{
  const struct __una_u32 *ptr = (const struct __una_u32 *)p;
  return ptr->x;
}

static inline uint64_t __get_unaligned_cpu64(const void *p)
{
  const struct __una_u64 *ptr = (const struct __una_u64 *)p;
  return ptr->x;
}

static inline void __put_unaligned_cpu16(uint16_t val, void *p)
{
  struct __una_u16 *ptr = (struct __una_u16 *)p;
  ptr->x = val;
}

static inline void __put_unaligned_cpu32(uint32_t val, void *p)
{
  struct __una_u32 *ptr = (struct __una_u32 *)p;
  ptr->x = val;
}

static inline void __put_unaligned_cpu64(uint64_t val, void *p)
{
  struct __una_u64 *ptr = (struct __una_u64 *)p;
  ptr->x = val;
}


static inline uint16_t get_unaligned_le16(const void *p)
{
	return le16_to_cpu(__get_unaligned_t(__le16, p));
}

static inline uint32_t get_unaligned_le32(const void *p)
{
	return le32_to_cpu(__get_unaligned_t(__le32, p));
}

static inline uint64_t get_unaligned_le64(const void *p)
{
	return le64_to_cpu(__get_unaligned_t(__le64, p));
}

static inline void put_unaligned_le16(uint16_t val, void *p)
{
	__put_unaligned_t(__le16, cpu_to_le16(val), p);
}

static inline void put_unaligned_le32(uint32_t val, void *p)
{
	__put_unaligned_t(__le32, cpu_to_le32(val), p);
}

static inline void put_unaligned_le64(uint64_t val, void *p)
{
	__put_unaligned_t(__le64, cpu_to_le64(val), p);
}

static inline uint16_t get_unaligned_be16(const void *p)
{
	return be16_to_cpu(__get_unaligned_t(__be16, p));
}

static inline uint32_t get_unaligned_be32(const void *p)
{
	return be32_to_cpu(__get_unaligned_t(__be32, p));
}

static inline uint64_t get_unaligned_be64(const void *p)
{
	return be64_to_cpu(__get_unaligned_t(__be64, p));
}

static inline void put_unaligned_be16(uint16_t val, void *p)
{
	__put_unaligned_t(__be16, cpu_to_be16(val), p);
}

static inline void put_unaligned_be32(uint32_t val, void *p)
{
	__put_unaligned_t(__be32, cpu_to_be32(val), p);
}

static inline void put_unaligned_be64(uint64_t val, void *p)
{
	__put_unaligned_t(__be64, cpu_to_be64(val), p);
}

static inline uint32_t __get_unaligned_be24(const uint8_t *p)
{
	return p[0] << 16 | p[1] << 8 | p[2];
}

static inline uint32_t get_unaligned_be24(const void *p)
{
	return __get_unaligned_be24(p);
}

static inline uint32_t __get_unaligned_le24(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16;
}

static inline uint32_t get_unaligned_le24(const void *p)
{
	return __get_unaligned_le24(p);
}

static inline void __put_unaligned_be24(const uint32_t val, uint8_t *p)
{
	*p++ = (val >> 16) & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = val & 0xff;
}

static inline void put_unaligned_be24(const uint32_t val, void *p)
{
	__put_unaligned_be24(val, p);
}

static inline void __put_unaligned_le24(const uint32_t val, uint8_t *p)
{
	*p++ = val & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = (val >> 16) & 0xff;
}

static inline void put_unaligned_le24(const uint32_t val, void *p)
{
	__put_unaligned_le24(val, p);
}

static inline void __put_unaligned_be48(const uint64_t val, uint8_t *p)
{
	*p++ = (val >> 40) & 0xff;
	*p++ = (val >> 32) & 0xff;
	*p++ = (val >> 24) & 0xff;
	*p++ = (val >> 16) & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = val & 0xff;
}

static inline void put_unaligned_be48(const uint64_t val, void *p)
{
	__put_unaligned_be48(val, p);
}

static inline uint64_t __get_unaligned_be48(const uint8_t *p)
{
	return (uint64_t)p[0] << 40 | (uint64_t)p[1] << 32 | (uint64_t)p[2] << 24 |
		p[3] << 16 | p[4] << 8 | p[5];
}

static inline uint64_t get_unaligned_be48(const void *p)
{
	return __get_unaligned_be48(p);
}

#endif /* __LINUX_UNALIGNED_H */