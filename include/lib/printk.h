#pragma once

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

// Initialize printing subsystem
void printk_register_backend(const printk_backend_t *backend);

/**
 * @funtion printk_auto_configure(2) - setup registered printk backedns
 * @param payload payload passed to init()
 * @param reinit status to check if
 */
void printk_auto_configure(void *payload, int reinit);
int printk_set_sink(const char *backend_name);
void printk_shutdown(void);
// Enable asynchronous printk logging (spawns background consumer thread).
void printk_init_async(void);

#define pr_emerg(fmt, ...) printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KERN_DEBUG fmt, ##__VA_ARGS__)