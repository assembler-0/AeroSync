#ifndef ARCLINE_LOG_H
#define ARCLINE_LOG_H

#include <stddef.h>
#include <stdint.h>

// Log levels (modeled after Linux)
#define KLOG_EMERG   0
#define KLOG_ALERT   1
#define KLOG_CRIT    2
#define KLOG_ERR     3
#define KLOG_WARNING 4
#define KLOG_NOTICE  5
#define KLOG_INFO    6
#define KLOG_DEBUG   7

void log_init(void);

// Set the minimum level stored in the ring buffer (inclusive)
void log_set_level(int level);
int  log_get_level(void);

// Console sink management
typedef void (*log_sink_putc_t)(char c);
void log_set_console_sink(log_sink_putc_t sink);
void log_set_console_level(int level);
int  log_get_console_level(void);

// Write a complete, already formatted message (no implicit newline added)
// Returns number of bytes accepted (may be truncated to ring capacity)
int log_write_str(int level, const char *msg);

// Read next record as a string. Returns length copied or 0 if none available.
// If out_level != NULL, stores the record level.
int log_read(char *out_buf, int out_buf_len, int *out_level);

#endif // ARCLINE_LOG_H
