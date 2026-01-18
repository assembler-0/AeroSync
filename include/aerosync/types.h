#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef long ssize_t;
typedef long ptrdiff_t;

#define ULONG_MAX (~0UL)
#define UINT_MAX  (~0U)
#define INT_MAX   ((int)(UINT_MAX >> 1))
#define LONG_MAX  ((long)(ULONG_MAX >> 1))

#define fn(ret, name, ...) ret (*name)(__VA_ARGS__)
#define fnd(ret, name, ...) typedef fn(ret, name, __VA_ARGS__)
