#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <kernel/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * VoidFrameX Enhanced Command-Line Parser
 * 
 * Supports:
 * - Registered options (type-checked)
 * - Unregistered options (key=value or flags)
 * - Quoted strings with spaces: key="value with spaces"
 * - Escape sequences: key="value with \"quotes\""
 */

typedef enum {
    CMDLINE_TYPE_FLAG = 0,   /* present or not */
    CMDLINE_TYPE_STRING = 1, /* key=value */
} cmdline_type_t;

/**
 * Register a known option. 
 * If registered, the parser ensures it matches the expected type.
 */
int cmdline_register_option(const char *key, cmdline_type_t type);

/**
 * Parse a raw command-line string.
 * This can be called multiple times; results are cumulative.
 */
int cmdline_parse(const char *cmdline);

/**
 * Query if a flag is present.
 * Works for both registered flags and unregistered tokens.
 */
int cmdline_get_flag(const char *key);

/**
 * Query for a string value.
 * returns NULL if key not found or has no value.
 */
const char *cmdline_get_string(const char *key);

/**
 * Iterate over all parsed options (useful for debug/logging).
 */
typedef void (*cmdline_iter_t)(const char *key, const char *value, void *priv);
void cmdline_for_each(cmdline_iter_t iter, void *priv);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_CMDLINE_H */