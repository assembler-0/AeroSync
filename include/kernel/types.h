#pragma once

#include "stdarg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef long ssize_t;
typedef long ptrdiff_t;

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))