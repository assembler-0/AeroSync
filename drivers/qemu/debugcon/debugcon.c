#include <arch/x86_64/io.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <lib/printk.h>

int debugcon_probe(void) {
  return 1; // assume success
}

int debugcon_is_initialized(void) {
  return 1;
}

static void debugcon_backend_putc(char c, int level) {
  (void) level;
  outb(QEMU_BOCHS_DEBUGCON_BASE, c);
}

static void debugcon_backend_write(const char *buf, size_t len, int level) {
  (void) level;
  for (size_t i = 0; i < len; i++) {
    outb(QEMU_BOCHS_DEBUGCON_BASE, buf[i]);
  }
}

static printk_backend_t debugcon_backend = {
  .name = "debugcon",
  .priority = 30,
  .putc = debugcon_backend_putc,
  .write = debugcon_backend_write,
  .probe = debugcon_probe,
  .init = generic_backend_init,
  .cleanup = nullptr,
  .is_active = debugcon_is_initialized
};

printk_backend_t *debugcon_get_backend(void) {
  return &debugcon_backend;
}
