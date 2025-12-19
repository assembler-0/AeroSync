#pragma once

#include <lib/printk.h>

#define QEMU_BOCHS_DEBUGCON_BASE 0xE9

int debugcon_probe(void);
void debugcon_putc(const char c);
const printk_backend_t* debugcon_get_backend(void);
