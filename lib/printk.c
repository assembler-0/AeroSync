#include <drivers/uart/serial.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <linearfb/linearfb.h>

static printk_backend_t fb_backend = {
  .name = "linearfb",
  .priority = 100,
  .putc = linearfb_console_putc,
  .probe = linearfb_probe,
};

static printk_backend_t serial_backend = {
  .name = "serial",
  .priority = 50,
  .putc = serial_write_char,
  .probe = serial_probe,
};

static printk_backend_t *printk_backends[] = {
  &fb_backend,
  &serial_backend,
};

static int num_backends = sizeof(printk_backends) / sizeof(printk_backends[0]);

void printk_init_auto(void) {
  printk_backend_t target = {0};
  int last_priority = -1;
  for (int i = 0; i < num_backends; i++) {
    if (printk_backends[i] &&
       printk_backends[i]->priority > last_priority &&
       printk_backends[i]->probe()
      ) {
      target = *printk_backends[i];
      last_priority = printk_backends[i]->priority;
    }
  }
  printk_init(target.putc);
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

void printk_init(const log_sink_putc_t backend) {
  log_init(backend);
}