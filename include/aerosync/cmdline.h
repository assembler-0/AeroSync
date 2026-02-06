#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <aerosync/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * AeroSync Enhanced Command-Line Parser
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
    CMDLINE_TYPE_INT = 2,    /* key=123 */
    CMDLINE_TYPE_BOOL = 3,   /* key=yes|no|1|0|true|false */
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
int cmdline_parse(char *cmdline);

/**
* Query if a flag is present.
* Works for both registered flags and unregistered tokens.
*/
int cmdline_get_flag(const char *key);

#ifdef CONFIG_CMDLINE_PARSER

/**
 * Query if a key is present in the command line (either as flag or key=value).
 */
int cmdline_has_option(const char *key);

/**
 * Query for a string value.
 * returns nullptr if key not found or has no value.
 */
const char *cmdline_get_string(const char *key);

/**
 * Query for an integer value.
 * Returns default_val if key not found or invalid.
 */
long long cmdline_get_int(const char *key, long long default_val);

/**
 * Query for an unsigned integer value.
 */
unsigned long long cmdline_get_uint(const char *key, unsigned long long default_val);

/**
 * Query for a boolean value.
 * Supports: yes/no, 1/0, true/false, on/off.
 * Returns default_val if key not found or invalid.
 */
int cmdline_get_bool(const char *key, int default_val);

/**
 * Iterate over all parsed options (useful for debug/logging).
 */
typedef void (*cmdline_iter_t)(const char *key, const char *value, void *priv);
void cmdline_for_each(cmdline_iter_t iter, void *priv);


#endif


#ifdef __cplusplus
}
#endif

#endif /* KERNEL_CMDLINE_H */