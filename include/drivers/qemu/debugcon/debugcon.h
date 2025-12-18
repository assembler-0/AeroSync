#pragma once

#define QEMU_BOCHS_DEBUGCON_BASE 0xE9

int debugcon_probe(void);
void debugcon_putc(const char c);
