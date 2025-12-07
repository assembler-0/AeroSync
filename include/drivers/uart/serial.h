#pragma once

#include <kernel/types.h>

#define COM1 0x3f8   // COM1
#define COM2 0x2f8   // COM2
#define COM3 0x3e8   // COM3
#define COM4 0x2e8   // COM4

// Initialize serial port (defaults to COM1)
int serial_init(void);

// Initialize specific serial port
int serial_init_port(uint16_t port);

// Status functions
int serial_transmit_empty(void);
int serial_data_available(void);

// Character I/O
void serial_write_char(char c);
int serial_read_char(void);

// String I/O
int serial_write(const char* str);
int serial_readline(char* buffer, int max_length);

// Number output
void serial_write_hex(uint64_t value);
void serial_write_dec(uint64_t value);