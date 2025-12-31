#include <kernel/cmdline.h>
#include <lib/string.h>
#include <kernel/ctype.h>
#include <stddef.h>
#include <stdint.h>

#define CMDLINE_MAX_OPTS 64
#define CMDLINE_MAX_KEYLEN 64
#define CMDLINE_MAX_CMDLEN 1024
#define CMDLINE_MAX_TOKENS 128
#define CMDLINE_MAX_VAL_LEN 256

struct cmdline_entry {
    char key[CMDLINE_MAX_KEYLEN];
    cmdline_type_t type;
    int present;
    /* For string options we store the value in a fixed buffer to avoid any
     * runtime allocation. value points either to value_buf or NULL. */
    char value_buf[CMDLINE_MAX_VAL_LEN];
    char *value;
};

static struct cmdline_entry cmdline_table[CMDLINE_MAX_OPTS];
static int cmdline_table_count = 0;

int cmdline_register_option(const char *key, cmdline_type_t type) {
    if (!key)
        return -1;
    if (cmdline_table_count >= CMDLINE_MAX_OPTS)
        return -1;
    /* ensure key length fits */
    size_t len = 0;
    while (key[len] && len < CMDLINE_MAX_KEYLEN)
        len++;
    if (key[len])
        return -1; /* too long */

    /* prevent duplicates */
    for (int i = 0; i < cmdline_table_count; ++i) {
        if (strcmp(cmdline_table[i].key, key) == 0)
            return -1;
    }

    strncpy(cmdline_table[cmdline_table_count].key, key, CMDLINE_MAX_KEYLEN);
    cmdline_table[cmdline_table_count].type = type;
    cmdline_table[cmdline_table_count].present = 0;
    cmdline_table[cmdline_table_count].value = NULL;
    cmdline_table[cmdline_table_count].value_buf[0] = '\0';
    cmdline_table_count++;
    return 0;
}

static struct cmdline_entry *find_entry(const char *key) {
    for (int i = 0; i < cmdline_table_count; ++i) {
        if (strcmp(cmdline_table[i].key, key) == 0)
            return &cmdline_table[i];
    }
    return NULL;
}

/* Tokenize into a static buffer and token pointer array. This avoids any
 * heap allocation. Returns the number of tokens placed into `out_tokens`.
 * Tokens are NUL-terminated in the local `buf`. */
static int tokenize_static(const char *s, char *buf, size_t buf_len,
                           char *out_tokens[], int max_tokens) {
    if (!s || !buf || buf_len == 0 || !out_tokens)
        return 0;

    /* copy input into buf with truncation guard */
    size_t i = 0;
    for (; i + 1 < buf_len && s[i]; ++i)
        buf[i] = s[i];
    buf[i] = '\0';

    int count = 0;
    char *p = buf;
    while (*p && count < max_tokens) {
        /* skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *start = NULL;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            start = p;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    /* support simple backslash escaping: move past both */
                    p += 2;
                } else {
                    p++;
                }
            }
            if (*p == quote) {
                *p = '\0';
                p++;
            }
            out_tokens[count++] = start;
        } else {
            start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) {
                *p = '\0';
                p++;
            }
            out_tokens[count++] = start;
        }
    }

    return count;
}

int cmdline_parse(const char *cmdline) {
    if (!cmdline) return 0;

    /* Static token buffers */
    static char buf[CMDLINE_MAX_CMDLEN];
    static char *tokens[CMDLINE_MAX_TOKENS];

    int tcount = tokenize_static(cmdline, buf, sizeof(buf), tokens, CMDLINE_MAX_TOKENS);
    int parsed = 0;

    for (int i = 0; i < tcount; ++i) {
        char *tok = tokens[i];
        if (!tok || !*tok) continue;
        /* Look for key=value */
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            const char *key = tok;
            const char *val = eq + 1;
            struct cmdline_entry *e = find_entry(key);
            if (e && e->type == CMDLINE_TYPE_STRING) {
                /* copy into fixed buffer with truncation */
                size_t j = 0;
                for (; j + 1 < CMDLINE_MAX_VAL_LEN && val[j]; ++j)
                    e->value_buf[j] = val[j];
                e->value_buf[j] = '\0';
                e->value = e->value_buf;
                e->present = 1;
                parsed++;
            }
        } else {
            /* flag style */
            struct cmdline_entry *e = find_entry(tok);
            if (e && e->type == CMDLINE_TYPE_FLAG) {
                e->present = 1;
                parsed++;
            }
        }
    }

    return parsed;
}

int cmdline_get_flag(const char *key) {
    struct cmdline_entry *e = find_entry(key);
    if (!e) return 0;
    return e->present ? 1 : 0;
}

const char *cmdline_get_string(const char *key) {
    struct cmdline_entry *e = find_entry(key);
    if (!e) return NULL;
    return e->value;
}
