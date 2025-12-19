#include <arch/x64/io.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <lib/printk.h>

int debugcon_probe(void) {
	return 1; // assume success
}

void debugcon_putc(const char c) {
	outb(QEMU_BOCHS_DEBUGCON_BASE, c);
}

static printk_backend_t debugcon_backend = {
    .name = "debugcon",
    .priority = 30,
    .putc = debugcon_putc,
    .probe = debugcon_probe,
    .init = generic_backend_init,
    .cleanup = NULL,
};

const printk_backend_t* debugcon_get_backend(void) {
    return &debugcon_backend;
}
