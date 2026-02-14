#pragma once

/* i know this is not the best way, but hey, it works  */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef uint32_t dev_t;
typedef long ssize_t;
typedef long ptrdiff_t;
typedef char* cstring;
typedef uintptr_t usize;
typedef long time_t;
typedef uint16_t char16_t;

#define ULONG_MAX (~0UL)
#define UINT_MAX  (~0U)
#define INT_MAX   ((int)(UINT_MAX >> 1))
#define LONG_MAX  ((long)(ULONG_MAX >> 1))

#define IDX_COUNT(arr) (sizeof(arr) / sizeof(arr[0]))
#define IDX_COUNT_HALF(arr) (IDX_COUNT(arr) / 2)

#define __STRINGIFY(x) #x
#define STRINGIFY(x) __STRINGIFY(x)

#define fn(ret, name, ...) ret (*name)(__VA_ARGS__)
#define fnd(ret, name, ...) typedef fn(ret, name, __VA_ARGS__)

/* Define restrict keyword for kernel environment */
#ifndef restrict
#define restrict __restrict__
#endif
