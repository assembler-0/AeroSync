/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/uart/serial_core.c
 * @brief Generic UART core for TTY and Console integration
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/sysintf/tty.h>
#include <drivers/uart/serial.h>
#include <lib/string.h>
#include <lib/log.h>

#define MAX_UART_PORTS 8
static struct uart_port *uart_ports[MAX_UART_PORTS];
static spinlock_t uart_ports_lock = SPINLOCK_INIT;

/* --- TTY Operations Glue --- */

static int uart_tty_open(struct tty_struct *tty) {
  struct uart_port *port = tty->driver_data;
  if (!port || !port->ops->setup) return -ENODEV;

  irq_flags_t flags = spinlock_lock_irqsave(&port->lock);
  int ret = port->ops->setup(port);
  if (ret == 0) {
    port->tty = tty;
    port->ops->enable_rx_irq(port);
  }
  spinlock_unlock_irqrestore(&port->lock, flags);

  return ret;
}

static void uart_tty_close(struct tty_struct *tty) {
  struct uart_port *port = tty->driver_data;
  if (!port) return;

  irq_flags_t flags = spinlock_lock_irqsave(&port->lock);
  port->ops->disable_rx_irq(port);
  port->ops->disable_tx_irq(port);
  if (port->ops->shutdown) port->ops->shutdown(port);
  port->tty = nullptr;
  spinlock_unlock_irqrestore(&port->lock, flags);
}

static ssize_t uart_tty_write(struct tty_struct *tty, const void *buf, size_t count) {
  struct uart_port *port = tty->driver_data;
  if (!port) return -ENODEV;

  const char *cbuf = buf;
  size_t i;

  irq_flags_t flags = spinlock_lock_irqsave(&port->lock);
  for (i = 0; i < count; i++) {
    /* If port is in polling mode or TX FIFO is full, we might need to wait
     * or trigger an interrupt-driven transfer. For now, simple poll-write.
     */
    port->ops->poll_putc(port, cbuf[i]);
  }
  spinlock_unlock_irqrestore(&port->lock, flags);

  return count;
}

static struct tty_operations uart_tty_ops = {
  .priority = TTY_PRIORITY_NORMAL,
  .open = uart_tty_open,
  .close = uart_tty_close,
  .write = uart_tty_write,
};

/* --- Printk Backend Wrapper --- */

static void uart_console_putc(char c, int level) {
  (void) level;
  /* Find the console port. In a real system, this would be a specific port. */
  for (int i = 0; i < MAX_UART_PORTS; i++) {
    if (uart_ports[i] && uart_ports[i]->is_console) {
      uart_ports[i]->ops->poll_putc(uart_ports[i], c);
      return;
    }
  }
}

static void uart_console_write(const char *buf, size_t len, int level) {
  (void) level;
  struct uart_port *port = nullptr;
  for (int i = 0; i < MAX_UART_PORTS; i++) {
    if (uart_ports[i] && uart_ports[i]->is_console) {
      port = uart_ports[i];
      break;
    }
  }

  if (!port) return;

  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '\n') port->ops->poll_putc(port, '\r');
    port->ops->poll_putc(port, buf[i]);
  }
}

static int uart_console_is_active(void) {
  for (int i = 0; i < MAX_UART_PORTS; i++) {
    if (uart_ports[i] && uart_ports[i]->is_console) {
      return 1;
    }
  }
  return 0;
}

static printk_backend_t uart_printk_backend = {
  .name = "uart-console",
  .priority = 100,
  .putc = uart_console_putc,
  .write = uart_console_write,
  .is_active = uart_console_is_active
};

/* --- Public Core API --- */

int uart_register_port(struct uart_port *port) {
  irq_flags_t flags = spinlock_lock_irqsave(&uart_ports_lock);
  int line = -1;
  for (int i = 0; i < MAX_UART_PORTS; i++) {
    if (!uart_ports[i]) {
      line = i;
      uart_ports[i] = port;
      break;
    }
  }
  spinlock_unlock_irqrestore(&uart_ports_lock, flags);

  if (line == -1) return -ENOSPC;

  port->line = line;
  spinlock_init(&port->lock);

  /* Register with TTY subsystem */
  port->dev = tty_register_device(&uart_tty_ops, port)->private_data;

  /* If this is the first port, make it the console by default for now */
  if (line == 0) {
    port->is_console = true;
    printk_register_backend(&uart_printk_backend);
  }

  return 0;
}

void uart_unregister_port(struct uart_port *port) {
  irq_flags_t flags = spinlock_lock_irqsave(&uart_ports_lock);
  if (port->line >= 0 && port->line < MAX_UART_PORTS) {
    uart_ports[port->line] = nullptr;
  }
  spinlock_unlock_irqrestore(&uart_ports_lock, flags);

  if (port->dev) {
    tty_unregister_device((struct char_device *) port->dev);
  }
}

void uart_handle_rx(struct uart_port *port) {
  if (!port->tty) return;

  int c;
  while ((c = port->ops->poll_getc(port)) != -1) {
    tty_receive_char(port->tty, (char) c);
  }
}

void uart_handle_tx(struct uart_port *port) {
  /* TODO: Pull from TTY write buffer */
  (void) port;
}
