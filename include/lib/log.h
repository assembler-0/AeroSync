#pragma once

// Log levels (modeled after Linux)
#define KLOG_EMERG   0
#define KLOG_ALERT   1
#define KLOG_CRIT    2
#define KLOG_ERR     3
#define KLOG_WARNING 4
#define KLOG_NOTICE  5
#define KLOG_INFO    6
#define KLOG_DEBUG   7

// Console sink management
fnd(void, log_sink_putc_t, char c);

void log_init(log_sink_putc_t backend);

void log_set_console_sink(log_sink_putc_t sink);

/*Mark that the system is panicking to allow bypassing locks*/
void log_mark_panic(void);

/*Write a complete, already formatted message (no implicit newline added)
Returns number of bytes accepted (may be truncated to ring capacity)*/
int log_write_str(int level, const char *msg);

/*Read next record as a string. Returns length copied or 0 if none available.
If out_level != nullptr, stores the record level.*/
int log_read(char *out_buf, int out_buf_len, int *out_level);

/*Optional runtime debug control: by default DEBUG may be off even if
the numeric level exists; use these helpers to enable/disable it at
runtime without changing the numeric log level used for other messages.*/
void log_enable_debug(void);
void log_disable_debug(void);
int  log_is_debug_enabled(void);

#ifdef ASYNC_PRINTK
/*If the console sink is known to be async-capable (i.e. slow but safe to
defer to the klogd consumer), callers can hint the logger to avoid
synchronous console emission during early boot:*/
void log_set_console_async_hint(int is_async);

/*Try to spawn the background klogd consumer now. Returns non-zero if the
consumer was started (or was already running). This allows callers to
attempt enabling async printing when the scheduler becomes available.*/
int log_try_init_async(void);

/*Start asynchronous logging consumer (klogd). Safe to call once after
scheduler is up. Subsequent calls are no-ops.*/
int __must_check log_init_async(void);
#endif /* ASYNC_PRINTK */