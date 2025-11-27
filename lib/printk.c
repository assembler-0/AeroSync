#include <drivers/uart/serial.h>
#include <lib/printk.h>
#include <kernel/types.h>
#include <lib/log.h>

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
                        buf[i++] =
                            (digit < 10) ? '0' + digit : 'a' + digit - 10;
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
                        buf[i++] =
                            (digit < 10) ? '0' + digit : 'a' + digit - 10;
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

static int printk_vroute(int level, const char *fmt, va_list args) {
    // Format into buffer then send to log subsystem
    printk_buf_ptr = printk_buf;
    printk_buf_rem = (int)sizeof(printk_buf);
    int cnt = do_printf(buf_putc, fmt, args);
    // NUL terminate
    *printk_buf_ptr = '\0';
    log_write_str(level, printk_buf);
    return cnt;
}

int vfprintk(int fd, const char *fmt, va_list args) {
    if (!fmt)
        return -1;

    int level = (fd == STDERR_FD) ? KLOG_ERR : KLOG_INFO;
    return printk_vroute(level, fmt, args);
}

int vprintk(const char *fmt, va_list args) {
    return vfprintk(STDOUT_FD, fmt, args);
}

int fprintk(int fd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vfprintk(fd, fmt, args);
    va_end(args);
    return ret;
}

// Non-variadic version for testing
int printk_simple(const char *fmt) {
    if (!fmt)
        return -1;

    while (*fmt) {
        serial_write_char(*fmt++);
    }
    return 0;
}

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vprintk(fmt, args);
    va_end(args);
    return ret;
}

void printk_init(void) {
    // Initialize logging subsystem
    log_init();
    // Console sink defaults to serial; can be changed later
}