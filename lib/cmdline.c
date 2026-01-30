///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/cmdline.c
 * @brief Enhanced Command-Line Parser
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/cmdline.h>
#include <lib/string.h>
#include <aerosync/ctype.h>

#define MAX_OPTS 128
#define MAX_KEY_LEN 64
#define MAX_VAL_LEN 256
#define CMDLINE_BUF_SIZE 2048

struct cmdline_entry {
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
    cmdline_type_t type;
    int present;
    int is_registered;
};

static struct cmdline_entry entries[MAX_OPTS];
static int entry_count = 0;

static struct cmdline_entry *find_entry(const char *key) {
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].key, key) == 0)
            return &entries[i];
    }
    return nullptr;
}

static struct cmdline_entry *get_or_create_entry(const char *key) {
    struct cmdline_entry *e = find_entry(key);
    if (e) return e;

    if (entry_count >= MAX_OPTS) return nullptr;

    e = &entries[entry_count++];
    strncpy(e->key, key, MAX_KEY_LEN - 1);
    e->key[MAX_KEY_LEN - 1] = '\0';
    e->present = 0;
    e->is_registered = 0;
    e->value[0] = '\0';
    return e;
}

int cmdline_register_option(const char *key, cmdline_type_t type) {
    if (!key) return -1;

    struct cmdline_entry *e = get_or_create_entry(key);
    if (!e) return -1;

    e->type = type;
    e->is_registered = 1;
    return 0;
}

/**
 * Tokenize cmdline string handling quotes and escapes.
 * Returns next token and updates *pos.
 */
static char *next_token(char **pos) {
    char *p = *pos;
    while (*p && isspace((unsigned char)*p)) p++;

    if (!*p) {
        *pos = p;
        return nullptr;
    }

    char *token_start = p;
    char *token_write = p;
    char quote = 0;

    while (*p) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
                p++;
                continue;
            }
            if (*p == '\\' && p[1]) p++; // Skip escape char
            *token_write++ = *p++;
        } else {
            if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            }
            if (isspace((unsigned char)*p)) {
                p++;
                break;
            }
            *token_write++ = *p++;
        }
    }

    *token_write = '\0';
    *pos = p;
    return token_start;
}

int cmdline_parse(const char *cmdline) {
    if (!cmdline) return 0;

    static char parse_buf[CMDLINE_BUF_SIZE];
    strncpy(parse_buf, cmdline, CMDLINE_BUF_SIZE - 1);
    parse_buf[CMDLINE_BUF_SIZE - 1] = '\0';

    char *p = parse_buf;
    char *token;
    int parsed = 0;

    while ((token = next_token(&p)) != nullptr) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char *key = token;
            char *val = eq + 1;

            struct cmdline_entry *e = get_or_create_entry(key);
            if (e) {
                strncpy(e->value, val, MAX_VAL_LEN - 1);
                e->value[MAX_VAL_LEN - 1] = '\0';
                e->present = 1;
                parsed++;
            }
        } else {
            // Flag
            struct cmdline_entry *e = get_or_create_entry(token);
            if (e) {
                e->present = 1;
                parsed++;
            }
        }
    }

    return parsed;
}

int cmdline_get_flag(const char *key) {
    struct cmdline_entry *e = find_entry(key);
    return (e && e->present) ? 1 : 0;
}

const char *cmdline_get_string(const char *key) {
    struct cmdline_entry *e = find_entry(key);
    if (!e || !e->present || e->value[0] == '\0') return nullptr;
    return e->value;
}

void cmdline_for_each(cmdline_iter_t iter, void *priv) {
    if (!iter) return;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].present) {
            iter(entries[i].key, entries[i].value[0] ? entries[i].value : nullptr, priv);
        }
    }
}