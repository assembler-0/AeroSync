# VoidFrameX

> VoidFrameX is a 64-bit x86_64 UEFI kernel written in C.

## Logging Backend Flexibility

The logging/printk backend can be switched at runtime. To use the framebuffer for printk/log output:

```c
#include <lib/log.h>
#include <lib/linearfb/linearfb.h>
log_set_console_sink(linearfb_log_putc);
```

To revert to serial output:

```c
#include <lib/log.h>
extern void serial_putc(char c);
log_set_console_sink(serial_putc);
```
