#pragma once

#include <kernel/types.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);  
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list args);