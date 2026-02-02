#pragma once

#include <aerosync/types.h>
#include <stdarg.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list args);
char *kvasprintf(const char *fmt, va_list args);