#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <kernel/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal, modular command-line parser.
 * - table driven: add entries to cmdline_register_option to extend parser
 * - supports flags (no value) and key=value options
 * - simple API to query boolean flags and string values
 *
 * Usage:
 *   cmdline_register_option("verbose", CMDLINE_TYPE_FLAG, NULL);
 *   cmdline_parse(cmdline_str);
 *   if (cmdline_get_flag("verbose")) { ... }
 */

typedef enum {
    CMDLINE_TYPE_FLAG = 0,   /* present or not */
    CMDLINE_TYPE_STRING = 1, /* key=value */
} cmdline_type_t;

/* Register an option that the parser knows about. Returns 0 on success. */
int cmdline_register_option(const char *key, cmdline_type_t type);

/* Parse a raw command-line (usually from bootloader). Returns number of
 * recognised tokens parsed. */
int cmdline_parse(const char *cmdline);

/* Query helpers */
int cmdline_get_flag(const char *key);       /* returns 1 if present, 0 otherwise */
const char *cmdline_get_string(const char *key); /* returns NULL if not present */

/* Convenience: known global flag accessor */
static inline int cmdline_verbose(void) { return cmdline_get_flag("verbose"); }

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_CMDLINE_H */
