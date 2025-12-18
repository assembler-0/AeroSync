#include <arch/x64/io.h>
#include <drivers/qemu/debugcon/debugcon.h>

int debugcon_probe(void) {
	return 1; // assume success
}

void debugcon_putc(const char c) {
	outb(QEMU_BOCHS_DEBUGCON_BASE, c);
}
