#ifndef _KERNEL_PRINTK_H
#define _KERNEL_PRINTK_H

#include <kernel/types.h>

// File descriptor constants
#define STDOUT_FD 1
#define STDERR_FD 2

// Print functions
int printk(const char *fmt, ...);
int fprintk(int fd, const char *fmt, ...);
int vprintk(const char *fmt, va_list args);
int vfprintk(int fd, const char *fmt, va_list args);

// Initialize printing subsystem
void printk_init(void);

#endif // _KERNEL_PRINTK_H