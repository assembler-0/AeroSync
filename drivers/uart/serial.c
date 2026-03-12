/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/uart/serial.c
 * @brief 8250/16550A UART hardware driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/asrx.h>
#include <drivers/uart/serial.h>
#include <arch/x86_64/io.h>

/* 8250 Register Offsets */
#define UART_RX          0    /* In:  Receive buffer */
#define UART_TX          0    /* Out: Transmit buffer */
#define UART_IER         1    /* I/O: Interrupt Enable Register */
#define UART_IIR         2    /* In:  Interrupt ID Register */
#define UART_FCR         2    /* Out: FIFO Control Register */
#define UART_LCR         3    /* I/O: Line Control Register */
#define UART_MCR         4    /* I/O: Modem Control Register */
#define UART_LSR         5    /* In:  Line Status Register */
#define UART_MSR         6    /* In:  Modem Status Register */
#define UART_SCR         7    /* I/O: Scratch Register */
#define UART_DLL         0    /* Out: Divisor Latch Low (DLAB=1) */
#define UART_DLM         1    /* Out: Divisor Latch High (DLAB=1) */

/* LCR bits */
#define UART_LCR_WLEN8   0x03 /* Word length: 8 bits */
#define UART_LCR_STOP1   0x00 /* Stop bits: 1 */
#define UART_LCR_PNONE   0x00 /* Parity: None */
#define UART_LCR_DLAB    0x80 /* Divisor Latch Access Bit */

/* LSR bits */
#define UART_LSR_DR      0x01 /* Data Ready */
#define UART_LSR_THRE    0x20 /* Transmit-hold-register empty */
#define UART_LSR_TEMT    0x40 /* Transmitter empty */

/* IER bits */
#define UART_IER_RDI     0x01 /* Enable receiver data interrupt */
#define UART_IER_THRI    0x02 /* Enable transmitter holding register int */

/* FCR bits */
#define UART_FCR_ENABLE  0x01 /* Enable FIFO */
#define UART_FCR_CLEAR_R 0x02 /* Clear receiver FIFO */
#define UART_FCR_CLEAR_X 0x04 /* Clear transmitter FIFO */
#define UART_FCR_T14     0xC0 /* Trigger level: 14 bytes */

/* MCR bits */
#define UART_MCR_DTR     0x01 /* Data Terminal Ready */
#define UART_MCR_RTS     0x02 /* Request To Send */
#define UART_MCR_OUT2    0x08 /* Output2 (Required for IRQs on some chips) */

static void serial8250_poll_putc(struct uart_port *port, char c) {
    uint16_t base = (uint16_t)port->iobase;
    int timeout = 100000;

    while (!(inb(base + UART_LSR) & UART_LSR_THRE) && --timeout > 0);
    outb(base + UART_TX, c);
}

static int serial8250_poll_getc(struct uart_port *port) {
    uint16_t base = (uint16_t)port->iobase;
    if (!(inb(base + UART_LSR) & UART_LSR_DR)) return -1;
    return inb(base + UART_RX);
}

static void serial8250_set_termios(struct uart_port *port, uint32_t baud, uint32_t cflag) {
    (void)cflag;
    uint16_t base = (uint16_t)port->iobase;
    uint16_t divisor = 115200 / baud;

    outb(base + UART_LCR, UART_LCR_DLAB);
    outb(base + UART_DLL, divisor & 0xFF);
    outb(base + UART_DLM, (divisor >> 8) & 0xFF);
    outb(base + UART_LCR, UART_LCR_WLEN8 | UART_LCR_STOP1 | UART_LCR_PNONE);
}

static int serial8250_setup(struct uart_port *port) {
    uint16_t base = (uint16_t)port->iobase;

    /* Disable all interrupts */
    outb(base + UART_IER, 0x00);

    /* Initialize hardware */
    serial8250_set_termios(port, 38400, 0);
    outb(base + UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR_R | UART_FCR_CLEAR_X | UART_FCR_T14);
    outb(base + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    return 0;
}

static void serial8250_enable_rx_irq(struct uart_port *port) {
    uint16_t base = (uint16_t)port->iobase;
    uint8_t ier = inb(base + UART_IER);
    outb(base + UART_IER, ier | UART_IER_RDI);
}

static void serial8250_disable_rx_irq(struct uart_port *port) {
    uint16_t base = (uint16_t)port->iobase;
    uint8_t ier = inb(base + UART_IER);
    outb(base + UART_IER, ier & ~UART_IER_RDI);
}

static const struct uart_ops serial8250_ops = {
    .setup = serial8250_setup,
    .poll_putc = serial8250_poll_putc,
    .poll_getc = serial8250_poll_getc,
    .set_termios = serial8250_set_termios,
    .enable_rx_irq = serial8250_enable_rx_irq,
    .disable_rx_irq = serial8250_disable_rx_irq,
};

static struct uart_port com1_port = {
    .iobase = COM1,
    .irq = 4,
    .ops = &serial8250_ops,
};

int serial_mod_init(void) {
    /* Register COM1 as a starting point */
    return uart_register_port(&com1_port);
}

void serial_mod_exit(void) {
    uart_unregister_port(&com1_port);
}

asrx_module_init(serial_mod_init);
asrx_module_exit(serial_mod_exit);
asrx_module_info(uart8250, KSYM_LICENSE_GPL);
