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

#define ULONG_MAX (~0UL)
#define UINT_MAX  (~0U)
#define INT_MAX   ((int)(UINT_MAX >> 1))
#define LONG_MAX  ((long)(ULONG_MAX >> 1))

#define __STRINGIFY(x) #x
#define STRINGIFY(x) __STRINGIFY(x)

#define fn(ret, name, ...) ret (*name)(__VA_ARGS__)
#define fnd(ret, name, ...) typedef fn(ret, name, __VA_ARGS__)
