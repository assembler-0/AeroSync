#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef long ssize_t;
typedef long ptrdiff_t;

#define fn(ret, name, ...) ret (*name)(__VA_ARGS__)
#define fnd(ret, name, ...) typedef fn(ret, name, __VA_ARGS__)
