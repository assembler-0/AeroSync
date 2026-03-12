/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/drivers/uart/serial.h
 * @brief Universal Asynchronous Receiver/Transmitter (UART) interface
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/tty.h>
#include <lib/printk.h>

/* Standard COM Port Addresses (x86) */
#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

struct uart_port;

/**
 * struct uart_ops - Hardware-level UART operations
 */
struct uart_ops {
    /* Power and initialization */
    int (*setup)(struct uart_port *port);
    void (*shutdown)(struct uart_port *port);

    /* Atomic I/O for Console/Printk */
    void (*poll_putc)(struct uart_port *port, char c);
    int (*poll_getc)(struct uart_port *port);

    /* Interrupt Management */
    void (*enable_tx_irq)(struct uart_port *port);
    void (*disable_tx_irq)(struct uart_port *port);
    void (*enable_rx_irq)(struct uart_port *port);
    void (*disable_rx_irq)(struct uart_port *port);

    /* Configuration */
    void (*set_termios)(struct uart_port *port, uint32_t baud, uint32_t cflag);
    
    /* Status and Control */
    unsigned int (*get_mctrl)(struct uart_port *port);
    void (*set_mctrl)(struct uart_port *port, unsigned int mctrl);
    unsigned int (*tx_empty)(struct uart_port *port);
};

/**
 * struct uart_port - Generic UART port abstraction
 */
struct uart_port {
    spinlock_t lock;
    uintptr_t iobase;
    int irq;
    unsigned int uartclk;
    unsigned int fifosize;
    
    struct tty_struct *tty;
    const struct uart_ops *ops;
    struct device *dev;
    
    int line;            /* Port index (e.g. 0 for ttyS0) */
    bool is_console;     /* Is this port the active printk console? */
    
    void *private_data;
};

/* --- Serial Core API --- */

/**
 * uart_register_port - Register a hardware UART port with the TTY system
 */
int uart_register_port(struct uart_port *port);

/**
 * uart_unregister_port - Unregister a UART port
 */
void uart_unregister_port(struct uart_port *port);

/**
 * uart_handle_rx - To be called by driver IRQ when data is available
 */
void uart_handle_rx(struct uart_port *port);

/**
 * uart_handle_tx - To be called by driver IRQ when TX FIFO is empty
 */
void uart_handle_tx(struct uart_port *port);

/* --- Legacy/Helper API --- */
int serial_init(void);
int serial_init_port(uint16_t port);
void serial_write_char(const char a);
int serial_is_initialized(void);
printk_backend_t *serial_get_backend(void);
int serial_probe(void);
