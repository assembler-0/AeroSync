#include <arch/x64/io.h>
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <linearfb/linearfb.h>
#include <drivers/qemu/debugcon/debugcon.h>

static printk_backend_t debugcon_backend = {
    .name = "debugcon",
    .priority = 30,
    .putc = debugcon_putc,
    .probe = debugcon_probe,
    .init = generic_backend_init,
};

static printk_backend_t fb_backend = {
    .name = "linearfb",
    .priority = 100,
    .putc = linearfb_console_putc,
    .probe = linearfb_probe,
    .init = linearfb_init_standard,
};

static printk_backend_t serial_backend = {
    .name = "serial",
    .priority = 50,
    .putc = serial_write_char,
    .probe = serial_probe,
    .init = serial_init_standard,
};

static printk_backend_t *printk_backends[] = {
    &fb_backend,
    &serial_backend,
    &debugcon_backend,
};

static int num_backends = sizeof(printk_backends) / sizeof(printk_backends[0]);

void printk_init_auto(void *payload)
{
    printk_backend_t *best = NULL;

    for (int i = 0; i < num_backends; i++) {
        printk_backend_t *b = printk_backends[i];
        if (!b)
            continue;

        if (!b->probe())
            continue;

        if (b->init(payload) != 0)
            continue;

        if (!best || b->priority > best->priority)
            best = b;
    }

    if (!best) {
        printk(KERN_ERR KERN_CLASS "no active printk backend\n");
        return;
    }

    printk(KERN_INFO KERN_CLASS
           "printk backend selected: %s (prio=%d)\n",
           best->name, best->priority);

    printk_init(best->putc);
}

void printk_init_async(void) {
  printk(KERN_CLASS "Enabling asynchronous printk...\n");
  log_init_async();
}

static const char *parse_level_prefix(const char *fmt, int *level_io) {
  if (!fmt)
    return fmt;
  // Preferred: $n$
  if (fmt[0] == '$' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '$') {
    if (level_io)
      *level_io = (fmt[1] - '0');
    return fmt + 3;
  }
  return fmt;
}

int vprintk(const char *fmt, va_list args) {
  if (!fmt)
    return -1;

  int level = KLOG_INFO;
  // Parse optional level prefix (e.g. "$3$")
  const char *real_fmt = parse_level_prefix(fmt, &level);

  char local_buf[512];
  int count = vsnprintf(local_buf, sizeof(local_buf), real_fmt, args);

  // Write to log subsystem
  log_write_str(level, local_buf);

  return count;
}

int printk(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintk(fmt, args);
  va_end(args);
  return ret;
}

void printk_init(const log_sink_putc_t backend) { log_init(backend); }
