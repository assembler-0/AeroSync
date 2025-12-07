#include <drivers/uart/serial.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <linearfb/linearfb.h>

// Buffering formatter that writes to the logging subsystem
typedef void (*output_func_t)(char c);

// Temporary per-call buffer for formatting
static char printk_buf[512];
static char *printk_buf_ptr;
static int printk_buf_rem;

static void buf_putc(char c) {
  if (printk_buf_rem > 1) {
    *printk_buf_ptr++ = c;
    printk_buf_rem--;
  }
}

static printk_backend_t fb_backend = {
  .name = "linearfb",
  .priority = 100,
  .putc = linearfb_console_putc,
};

static printk_backend_t serial_backend = {
  .name = "serial",
  .priority = 50,
  .putc = serial_write_char,
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
    if (printk_backends[i]->priority > last_priority && printk_backends[i]) {
      target = *printk_backends[i];
      last_priority = printk_backends[i]->priority;
    }
  }
  printk_init(target.putc);
}

// Simple printf implementation
static int do_printf(output_func_t output, const char *fmt, va_list args) {
  if (!output || !fmt)
    return -1;

  int count = 0;

  while (*fmt) {
    if (*fmt == '%' && *(fmt + 1)) {
      fmt++;
      switch (*fmt) {
      case 'u': {
        unsigned int val = va_arg(args, unsigned int);
        char buf[12];
        int i = 0;
        if (val == 0) {
          buf[i++] = '0';
        } else {
          while (val > 0) {
            buf[i++] = '0' + (val % 10);
            val /= 10;
          }
        }
        while (i > 0) {
          output(buf[--i]);
          count++;
        }
        break;
      }

      case 'd': {
        int val = va_arg(args, int);
        if (val < 0) {
          output('-');
          count++;
          val = -val;
        }
        char buf[12];
        int i = 0;
        if (val == 0) {
          buf[i++] = '0';
        } else {
          while (val > 0) {
            buf[i++] = '0' + (val % 10);
            val /= 10;
          }
        }
        while (i > 0) {
          output(buf[--i]);
          count++;
        }
        break;
      }

      case 'l': {
        if (*(fmt + 1) == 'l' && *(fmt + 2) == 'u') {
          fmt += 2;
          uint64_t val = va_arg(args, uint64_t);
          char buf[24];
          int i = 0;
          if (val == 0) {
            buf[i++] = '0';
          } else {
            while (val > 0) {
              buf[i++] = '0' + (val % 10);
              val /= 10;
            }
          }
          while (i > 0) {
            output(buf[--i]);
            count++;
          }
        } else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'd') {
          fmt += 2;
          int64_t val = va_arg(args, int64_t);
          if (val < 0) {
            output('-');
            count++;
            val = -val;
          }
          char buf[24];
          int i = 0;
          if (val == 0) {
            buf[i++] = '0';
          } else {
            while (val > 0) {
              buf[i++] = '0' + (val % 10);
              val /= 10;
            }
          }
          while (i > 0) {
            output(buf[--i]);
            count++;
          }
        } else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'x') {
          // 64-bit hex
          fmt += 2;
          uint64_t val = va_arg(args, uint64_t);
          char buf[17];
          int i = 0;
          if (val == 0) {
            buf[i++] = '0';
          } else {
            while (val > 0) {
              int digit = val & 0xF;
              buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
              val >>= 4;
            }
          }
          while (i > 0) {
            output(buf[--i]);
            count++;
          }
        }
        break;
      }

      case 'x': {
        unsigned int val = va_arg(args, unsigned int);
        char buf[9];
        int i = 0;

        if (val == 0) {
          buf[i++] = '0';
        } else {
          while (val > 0) {
            int digit = val & 0xF;
            buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
            val >>= 4;
          }
        }

        while (i > 0) {
          output(buf[--i]);
          count++;
        }
        break;
      }

      case 'p': {
        void *ptr = va_arg(args, void *);
        uint64_t val = (uint64_t)ptr;

        output('0');
        output('x');
        count += 2;

        char buf[17];
        int i = 0;

        if (val == 0) {
          buf[i++] = '0';
        } else {
          while (val > 0) {
            int digit = val & 0xF;
            buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
            val >>= 4;
          }
        }

        while (i > 0) {
          output(buf[--i]);
          count++;
        }
        break;
      }

      case 's': {
        const char *str = va_arg(args, const char *);
        if (!str)
          str = "(null)";

        while (*str) {
          output(*str++);
          count++;
        }
        break;
      }

      case 'c': {
        char c = (char)va_arg(args, int);
        output(c);
        count++;
        break;
      }

      case '%':
        output('%');
        count++;
        break;

      default:
        output('%');
        output(*fmt);
        count += 2;
        break;
      }
    } else {
      output(*fmt);
      count++;
    }
    fmt++;
  }

  return count;
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

  // Prepare buffer
  printk_buf_ptr = printk_buf;
  printk_buf_rem = (int)sizeof(printk_buf);

  // Format message
  int count = do_printf(buf_putc, real_fmt, args);

  // NUL terminate
  *printk_buf_ptr = '\0';

  // Write to log subsystem
  log_write_str(level, printk_buf);

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