///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/cmdline.c
 * @brief Production-Ready Command-Line Parser
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/cmdline.h>
#include <lib/string.h>
#include <aerosync/ctype.h>
#include <aerosync/export.h>

#ifndef CONFIG_USE_CMDLINE_PARSER
static cstring g_cmdline = nullptr;
#endif

#ifdef CONFIG_CMDLINE_PARSER

#ifndef CONFIG_CMDLINE_MAX_OPTS
#define CONFIG_CMDLINE_MAX_OPTS 128
#endif

#ifndef CONFIG_CMDLINE_MAX_KEY
#define CONFIG_CMDLINE_MAX_KEY 64
#endif

#ifndef CONFIG_CMDLINE_MAX_VAL
#define CONFIG_CMDLINE_MAX_VAL 256
#endif

#ifndef CONFIG_CMDLINE_BUF_SIZE
#define CONFIG_CMDLINE_BUF_SIZE 4096
#endif

struct cmdline_entry {
  char key[CONFIG_CMDLINE_MAX_KEY];
  char value[CONFIG_CMDLINE_MAX_VAL];
  cmdline_type_t type;
  int present;
  int is_registered;
};

static struct cmdline_entry entries[CONFIG_CMDLINE_MAX_OPTS];
static int entry_count = 0;

static struct cmdline_entry *find_entry(const char *key) {
  if (!key) return nullptr;
  for (int i = 0; i < entry_count; i++) {
    if (strcmp(entries[i].key, key) == 0)
      return &entries[i];
  }
  return nullptr;
}

static struct cmdline_entry *get_or_create_entry(const char *key) {
  struct cmdline_entry *e = find_entry(key);
  if (e) return e;

  if (entry_count >= CONFIG_CMDLINE_MAX_OPTS) return nullptr;

  e = &entries[entry_count++];
  strncpy(e->key, key, CONFIG_CMDLINE_MAX_KEY);
  e->present = 0;
  e->is_registered = 0;
  e->value[0] = '\0';
  return e;
}

/**
 * Tokenize cmdline string handling quotes and escapes.
 * Does NOT modify the original string, uses an internal write pointer
 * to strip quotes and handle escapes into the same buffer.
 * Returns next token and updates *pos.
 */
static char *next_token(char **pos) {
  static char token_buf[CONFIG_CMDLINE_MAX_VAL];
  char *p = *pos;
  while (*p && isspace((unsigned char)*p))
    p++;

  if (!*p) {
    *pos = p;
    return nullptr;
  }

  char *token_start = p;
  char quote = 0;
  bool escaped = false;

  // First pass: find token end
  char *token_end = p;
  while (*token_end) {
    if (escaped) {
      escaped = false;
      token_end++;
      continue;
    }
    if (*token_end == '\\') {
      escaped = true;
      token_end++;
      continue;
    }
    if (quote) {
      if (*token_end == quote)
        quote = 0;
      token_end++;
    } else {
      if (*token_end == '"' || *token_end == '\'') {
        quote = *token_end++;
      } else if (isspace((unsigned char)*token_end)) {
        break;
      } else {
        token_end++;
      }
    }
  }

  // Now process quotes/escapes into token_buf
  char *write = token_buf;
  char *write_end = token_buf + CONFIG_CMDLINE_MAX_VAL - 1;
  quote = 0;
  escaped = false;
  p = token_start;

  while (p < token_end && write < write_end) {
    if (escaped) {
      *write++ = *p++;
      escaped = false;
      continue;
    }
    if (*p == '\\') {
      escaped = true;
      p++;
      continue;
    }
    if (quote) {
      if (*p == quote) {
        quote = 0;
        p++;
      } else {
        *write++ = *p++;
      }
    } else {
      if (*p == '"' || *p == '\'') {
        quote = *p++;
      } else {
        *write++ = *p++;
      }
    }
  }

  *write = '\0';

  // Skip trailing whitespace for next call
  while (*token_end && isspace((unsigned char)*token_end))
    token_end++;
  *pos = token_end;

  return token_buf;
}

#endif /* CONFIG_CMDLINE_PARSER */

int cmdline_register_option(const char *key, cmdline_type_t type) {
#ifdef CONFIG_CMDLINE_PARSER
  if (!key) return -1;

  struct cmdline_entry *e = get_or_create_entry(key);
  if (!e) return -1;

  e->type = type;
  e->is_registered = 1;
#endif
  return 0; /* no-op for simple parser */
}
EXPORT_SYMBOL(cmdline_register_option);

int cmdline_parse(char *cmdline) {
#ifdef CONFIG_CMDLINE_PARSER
  if (!cmdline) return 0;

  static char parse_buf[CONFIG_CMDLINE_BUF_SIZE];
  
#ifdef CONFIG_CMDLINE_OVERRIDE
  if (strlen(CONFIG_CMDLINE_OVERRIDE) > 0) {
    cmdline = CONFIG_CMDLINE_OVERRIDE;
  }
#endif

  strncpy(parse_buf, cmdline, CONFIG_CMDLINE_BUF_SIZE);

#ifdef CONFIG_CMDLINE_APPEND
  if (strlen(CONFIG_CMDLINE_APPEND) > 0) {
    strlcat(parse_buf, " ", CONFIG_CMDLINE_BUF_SIZE);
    strlcat(parse_buf, CONFIG_CMDLINE_APPEND, CONFIG_CMDLINE_BUF_SIZE);
  }
#endif

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
        strncpy(e->value, val, CONFIG_CMDLINE_MAX_VAL);
        e->present = 1;
        parsed++;
      }
    } else {
      struct cmdline_entry *e = get_or_create_entry(token);
      if (e) {
        e->present = 1;
        parsed++;
      }
    }
  }

  return parsed;
#else
  g_cmdline = cmdline;
  return 0;
#endif
}
EXPORT_SYMBOL(cmdline_parse);

int cmdline_has_option(const char *key) {
#ifdef CONFIG_CMDLINE_PARSER
  struct cmdline_entry *e = find_entry(key);
  return (e && e->present) ? 1 : 0;
#else
  return find(g_cmdline, key);
#endif
}
EXPORT_SYMBOL(cmdline_has_option);

int cmdline_get_flag(const char *key) {
#ifdef CONFIG_CMDLINE_PARSER
  return cmdline_has_option(key);
#else
  return find(g_cmdline, key);
#endif
}
EXPORT_SYMBOL(cmdline_get_flag);

#ifdef CONFIG_CMDLINE_PARSER
const char *cmdline_get_string(const char *key) {
  struct cmdline_entry *e = find_entry(key);
  if (!e || !e->present || e->value[0] == '\0') return nullptr;
  return e->value;
}
EXPORT_SYMBOL(cmdline_get_string);

long long cmdline_get_int(const char *key, long long default_val) {
  const char *val = cmdline_get_string(key);
  if (!val) return default_val;
  return simple_strtoll(val, nullptr, 0);
}
EXPORT_SYMBOL(cmdline_get_int);

unsigned long long cmdline_get_uint(const char *key, unsigned long long default_val) {
  const char *val = cmdline_get_string(key);
  if (!val) return default_val;
  return simple_strtoull(val, nullptr, 0);
}
EXPORT_SYMBOL(cmdline_get_uint);

int cmdline_get_bool(const char *key, int default_val) {
  struct cmdline_entry *e = find_entry(key);
  if (!e || !e->present) return default_val;

  // If it's just a flag without value, it's true
  if (e->value[0] == '\0') return 1;

  const char *v = e->value;
  if (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 ||
      strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0) {
    return 1;
  }
  if (strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
      strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0) {
    return 0;
  }

  return default_val;
}
EXPORT_SYMBOL(cmdline_get_bool);

void cmdline_for_each(cmdline_iter_t iter, void *priv) {
  if (!iter) return;
  for (int i = 0; i < entry_count; i++) {
    if (entries[i].present) {
      iter(entries[i].key, entries[i].value[0] ? entries[i].value : nullptr, priv);
    }
  }
}
EXPORT_SYMBOL(cmdline_for_each);
#endif