///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/uart/serial.c
 * @brief serial UART printk backend
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <kernel/fkx/fkx.h>
#include <drivers/uart/serial.h>
#include <arch/x86_64/io.h>

// Serial port register offsets
#define SERIAL_DATA_REG     0
#define SERIAL_IER_REG      1
#define SERIAL_DIVISOR_LOW  0
#define SERIAL_DIVISOR_HIGH 1
#define SERIAL_FIFO_REG     2
#define SERIAL_LCR_REG      3
#define SERIAL_MCR_REG      4
#define SERIAL_LSR_REG      5
#define SERIAL_MSR_REG      6

#define SERIAL_LCR_DLAB     0x80
#define SERIAL_LCR_8BITS    0x03
#define SERIAL_LCR_1STOP    0x00
#define SERIAL_LCR_NOPARITY 0x00

#define SERIAL_LSR_DATA_READY    0x01
#define SERIAL_LSR_TRANSMIT_EMPTY 0x20

#define SERIAL_FIFO_ENABLE      0x01
#define SERIAL_FIFO_CLEAR_RX    0x02
#define SERIAL_FIFO_CLEAR_TX    0x04
#define SERIAL_FIFO_TRIGGER_14  0xC0

#define SERIAL_MCR_DTR          0x01
#define SERIAL_MCR_RTS          0x02
#define SERIAL_MCR_OUT2         0x08

static uint16_t serial_port = COM1;
static int serial_initialized = 0;

int serial_init_standard(void *unused) {
  (void)unused;
  if (serial_init() != 0)
    if (serial_init_port(COM2) != 0 || serial_init_port(COM3) != 0 ||
        serial_init_port(COM4) != 0) return -1;
  serial_initialized = 1;
  return 0;
}

static void serial_cleanup(void) {
    if (serial_initialized) {
        outb(serial_port + SERIAL_IER_REG, 0x00);
    }
}

int serial_is_initialized(void) {
    return serial_initialized;
}

static printk_backend_t serial_backend = {
    .name = "serial",
    .priority = 50,
    .putc = serial_write_char,
    .probe = serial_probe,
    .init = serial_init_standard,
    .cleanup = serial_cleanup,
    .is_active = serial_is_initialized
};

const printk_backend_t* serial_get_backend(void) {
    return &serial_backend;
}

static int serial_lsr_sane(uint16_t base) {
    uint8_t lsr = inb(base + 5);
    if (lsr == 0xFF || lsr == 0x00)
        return 0;
    return 1;
}

int serial_port_exists(uint16_t base) {
    uint8_t old = inb(base + 7);
    outb(base + 7, 0xA5);
    if (inb(base + 7) != 0xA5) goto fallback;
    outb(base + 7, 0x5A);
    if (inb(base + 7) != 0x5A) goto fallback;
    outb(base + 7, old);
    return 1;
fallback:
    outb(base + 7, old);
    return serial_lsr_sane(base);
}

int serial_probe(void) {
    uint16_t ports[] = {COM1, COM2, COM3, COM4};
    for (int i = 0; i < 4; i++) {
        if (serial_port_exists(ports[i])) return 1;
    }
    return 0;
}

int serial_init(void) {
    return serial_init_port(COM1);
}

int serial_init_port(uint16_t port) {
    serial_port = port;
    outb(port + SERIAL_IER_REG, 0x00);
    outb(port + SERIAL_LCR_REG, SERIAL_LCR_DLAB);
    outb(port + SERIAL_DIVISOR_LOW, 0x03);
    outb(port + SERIAL_DIVISOR_HIGH, 0x00);
    outb(port + SERIAL_LCR_REG, SERIAL_LCR_8BITS | SERIAL_LCR_NOPARITY | SERIAL_LCR_1STOP);
    outb(port + SERIAL_FIFO_REG, SERIAL_FIFO_ENABLE | SERIAL_FIFO_CLEAR_RX | SERIAL_FIFO_CLEAR_TX | SERIAL_FIFO_TRIGGER_14);
    outb(port + SERIAL_MCR_REG, SERIAL_MCR_DTR | SERIAL_MCR_RTS | SERIAL_MCR_OUT2);
    outb(port + SERIAL_MCR_REG, SERIAL_MCR_DTR | SERIAL_MCR_RTS | SERIAL_MCR_OUT2 | 0x10);
    outb(port + SERIAL_DATA_REG, 0xAE);
    if (inb(port + SERIAL_DATA_REG) != 0xAE) return -2;
    outb(port + SERIAL_MCR_REG, SERIAL_MCR_DTR | SERIAL_MCR_RTS | SERIAL_MCR_OUT2);
    serial_initialized = 1;
    return 0;
}

int serial_transmit_empty(void) {
    if (!serial_initialized) return 0;
    return inb(serial_port + SERIAL_LSR_REG) & SERIAL_LSR_TRANSMIT_EMPTY;
}

void serial_write_char(const char a) {
    if (!serial_initialized) return;
    int timeout = 65536;
    if (a == '\n') {
        while (!serial_transmit_empty() && --timeout > 0);
        if (timeout <= 0) return;
        outb(serial_port + SERIAL_DATA_REG, '\r');
        timeout = 65536;
    }
    while (!serial_transmit_empty() && --timeout > 0);
    if (timeout <= 0) return;
    outb(serial_port + SERIAL_DATA_REG, a);
}

int serial_mod_init(void) {
  printk_register_backend(serial_get_backend());
  return 0;
}

FKX_MODULE_DEFINE(
  serial,
  "0.0.1",
  "assembler-0",
  "Serial UART Module",
  0,
  FKX_PRINTK_CLASS,
  serial_mod_init,
  NULL
);
