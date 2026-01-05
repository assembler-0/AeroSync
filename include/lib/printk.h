#pragma once

#include <kernel/spinlock.h>
#include <kernel/types.h>

typedef struct printk_backend {
  const char *name;
  int priority;              // bigger = preferred
  fn(void, putc, char c);
  fn(int, probe, void);
  fn(int, init, void *payload);
  fn(void, cleanup, void);
  fn(int, is_active, void);
} printk_backend_t;

static int generic_backend_init(void *payload) { (void)payload; return 0; }

#define KERN_EMERG "$0$"
#define KERN_ALERT "$1$"
#define KERN_CRIT "$2$"
#define KERN_ERR "$3$"
#define KERN_WARNING "$4$"
#define KERN_NOTICE "$5$"
#define KERN_INFO "$6$"
#define KERN_DEBUG "$7$"

// Print functions
int printk(const char *fmt, ...);
int vprintk(const char *fmt, va_list args);

#define PRINTK_BACKEND_NAME(b) (b ? b->name : NULL)

// Initialize printing subsystem
void printk_register_backend(const printk_backend_t *backend);

/**
 * @function printk_auto_configure(2) - setup registered printk backedns
 * @param payload payload passed to init()
 * @param reinit status to check if should re-initialize or initialize
 */
void printk_auto_configure(void *payload, int reinit);

#define printk_init_early() printk_auto_configure(NULL, 0)
#define printk_init_late() printk_auto_configure(NULL, 1)

/**
 * @function printk_set_sink(1) - change printk backend
 * @param backend_name backend name, disable printk if recived NULL
 * @param cleanup clean up status
 */
int printk_set_sink(const char *backend_name, bool cleanup);
void printk_shutdown(void);

/**
 * @function printk_disable(0) - disable console output but keep ringbuffer logging
 */
void printk_disable(void);

/**
 * @function printk_enable(0) - re-enable console output to last active backend
 */
void printk_enable(void);

/**
 * @function printk_auto_select_backend(1) - select best backend
 * @param not exclude backend name
 * @exception printk_auto_select_backend will never return the current active backend
 */
const printk_backend_t *printk_auto_select_backend(const char *not);
// Enable asynchronous printk logging (spawns background consumer thread).
void printk_init_async(void);

typedef struct ratelimit_state {
  spinlock_t lock;
  int interval;      // interval in ms
  int burst;         // max messages in interval
  int printed;       // messages printed in current interval
  int missed;        // messages dropped
  uint64_t begin;    // interval start time in ns
} ratelimit_state_t;

#define RATELIMIT_STATE_INIT(name, interval_ms, burst_count) { \
  .lock = 0, \
  .interval = interval_ms, \
  .burst = burst_count, \
  .printed = 0, \
  .missed = 0, \
  .begin = 0 \
}

#define DEFINE_RATELIMIT_STATE(name, interval_ms, burst_count) \
  ratelimit_state_t name = RATELIMIT_STATE_INIT(name, interval_ms, burst_count)

int ___ratelimit(ratelimit_state_t *rs, const char *func);

#define printk_ratelimited(rs, fmt, ...) ({ \
  int __ret = 0; \
  if (___ratelimit(rs, __func__)) \
    __ret = printk(fmt, ##__VA_ARGS__); \
  __ret; \
})

#define pr_emerg_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug_ratelimited(rs, fmt, ...) printk_ratelimited(rs, KERN_DEBUG fmt, ##__VA_ARGS__)

#define pr_emerg(fmt, ...) printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KERN_DEBUG fmt, ##__VA_ARGS__)