#pragma once

/* i know this is not the best way, but hey, it works  */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t dev_t;
typedef long ssize_t;
typedef long ptrdiff_t;
typedef char* cstring;
typedef uintptr_t usize;
typedef long time_t;
typedef uint16_t char16_t;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define ULONG_MAX (~0UL)
#define UINT_MAX  (~0U)
#define INT_MAX   ((int)(UINT_MAX >> 1))
#define LONG_MAX  ((long)(ULONG_MAX >> 1))

#define IDX_COUNT(arr) (sizeof(arr) / sizeof(arr[0]))
#define IDX_COUNT_HALF(arr) (IDX_COUNT(arr) / 2)
#define ARRAY_SIZE(x) IDX_COUNT(x)

#define __STRINGIFY(x) #x
#define STRINGIFY(x) __STRINGIFY(x)

#define fn(ret, name, ...) ret (*name)(__VA_ARGS__)
#define fnd(ret, name, ...) typedef fn(ret, name, __VA_ARGS__)

#define USHRT_MAX	((unsigned short)~0U)
#define SHRT_MAX	((short)(USHRT_MAX >> 1))
#define SHRT_MIN	((short)(-SHRT_MAX - 1))
#define INT_MAX		((int)(~0U >> 1))
#define INT_MIN		(-INT_MAX - 1)
#define UINT_MAX	(~0U)
#define LONG_MAX	((long)(~0UL >> 1))
#define LONG_MIN	(-LONG_MAX - 1)
#define ULONG_MAX	(~0UL)
#define LLONG_MAX	((long long)(~0ULL >> 1))
#define LLONG_MIN	(-LLONG_MAX - 1)
#define ULLONG_MAX	(~0ULL)
#define UINTPTR_MAX	ULONG_MAX

#define BITS_PER_LONG 64
#define BITS_PER_LONG_LONG 64

