/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/tty.h
 * @brief TTY/Serial class interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/sysintf/char.h>
#include <aerosync/mutex.h>
#include <lib/ringbuf.h>

struct tty_struct;
struct tty_driver;

/**
 * struct tty_operations - Hardware-specific TTY operations
 */
struct tty_operations {
    int (*open)(struct tty_struct *tty);
    void (*close)(struct tty_struct *tty);
    ssize_t (*write)(struct tty_struct *tty, const void *buf, size_t count);
    int (*ioctl)(struct tty_struct *tty, uint32_t cmd, void *arg);
    void (*set_termios)(struct tty_struct *tty);
};

/**
 * struct tty_struct - Core TTY state
 */
struct tty_struct {
    struct char_device *cdev;
    const struct tty_operations *ops;
    
    ringbuf_t *read_buf;  /* Raw input from hardware */
    ringbuf_t *write_buf; /* Outgoing data */
    
    mutex_t lock;
    void *driver_data;
    
    /* Termios state (simplified) */
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
};

/**
 * tty_register_driver - Register a new TTY driver
 */
int tty_register_driver(struct tty_driver *driver);

/**
 * tty_receive_char - Call from IRQ to push data into TTY
 */
void tty_receive_char(struct tty_struct *tty, char c);

/**
 * tty_get_char_ops - Get the generic char_operations for TTYs
 */
const struct char_operations *tty_get_char_ops(void);

/* Legacy/Helper API */
struct char_device *tty_register_device(const struct char_operations *ops, void *private_data);
