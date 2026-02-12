/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/limine_modules.c
 * @brief Limine Module Manager (LMM) Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/limine_modules.h>
#include <mm/slub.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/errno.h>

#define LMM_MAX_PROBERS 16

static LIST_HEAD(g_lmm_entries);
static size_t g_lmm_count = 0;

static lmm_prober_fn g_lmm_probers[LMM_MAX_PROBERS];
static size_t g_lmm_prober_count = 0;

int lmm_register_prober(lmm_prober_fn prober) {
  if (g_lmm_prober_count >= LMM_MAX_PROBERS) {
    return -ENOMEM;
  }
  g_lmm_probers[g_lmm_prober_count++] = prober;
  return 0;
}

static lmm_type_t lmm_probe_file(const struct limine_file *file) {
  lmm_type_t best_type = LMM_TYPE_UNKNOWN;
  int best_score = -1;

#ifdef CONFIG_LMM_PROBE_EXTENSION_FIRST
  /* Simple extension-based probing if configured */
  const char *dot = strrchr(file->path, '.');
  if (dot) {
    if (strcmp(dot, ".fkx") == 0) {
      best_type = LMM_TYPE_FKX;
      best_score = 10; /* Low score for extension match */
    } else if (strcmp(dot, ".asrx") == 0) {
      best_type = LMM_TYPE_ASRX;
      best_score = 10;
    } else if (strcmp(dot, ".cpio") == 0) {
      best_type = LMM_TYPE_INITRD;
      best_score = 10;
    }
  }
#endif

  for (size_t i = 0; i < g_lmm_prober_count; i++) {
    lmm_type_t type = LMM_TYPE_UNKNOWN;
    int score = g_lmm_probers[i](file, &type);
    if (score > best_score) {
      best_score = score;
      best_type = type;
    }
  }

  return best_type;
}

int lmm_init(const struct limine_module_response *response) {
  if (!response) {
    return -EINVAL;
  }

  printk(KERN_INFO LMM_CLASS "Initializing with %lu modules\n", response->module_count);

  for (size_t i = 0; i < response->module_count; i++) {
    const struct limine_file *m = response->modules[i];
    struct lmm_entry *entry = kmalloc(sizeof(struct lmm_entry));
    if (!entry) {
      return -ENOMEM;
    }

    entry->file = m;
    entry->type = lmm_probe_file(m);
    entry->priority = 0; /* Default priority */
    entry->priv = nullptr;

    list_add_tail(&entry->list, &g_lmm_entries);
    g_lmm_count++;

    printk(KERN_DEBUG LMM_CLASS "Module [%zu] %s type=%d\n", i, m->path, entry->type);
  }

#ifdef CONFIG_LMM_SORT_BY_PRIORITY
  /* Sort by priority (descending) */
  if (g_lmm_count > 1) {
    bool swapped;
    do {
      swapped = false;
      struct lmm_entry *curr, *next;
      list_for_each_entry_safe(curr, next, &g_lmm_entries, list) {
        if (&next->list == &g_lmm_entries) break;
        if (curr->priority < next->priority) {
          list_move(&curr->list, &next->list);
          swapped = true;
          /* After move, we need to restart or be very careful. 
           * Simple approach: restart loop on swap. */
          break;
        }
      }
    } while (swapped);
  }
#endif

  return 0;
}

void lmm_for_each_module(lmm_type_t type, void (*callback)(struct lmm_entry *entry, void *data), void *data) {
  struct lmm_entry *entry;
  list_for_each_entry(entry, &g_lmm_entries, list) {
    if (type == LMM_TYPE_MAX || entry->type == type) {
      callback(entry, data);
    }
  }
}

struct lmm_entry *lmm_find_module(const char *name) {
  struct lmm_entry *entry;
  list_for_each_entry(entry, &g_lmm_entries, list) {
    const char *filename = strrchr(entry->file->path, '/');
    if (filename) filename++;
    else filename = entry->file->path;

    if (strcmp(filename, name) == 0) {
      return entry;
    }
  }
  return nullptr;
}

struct lmm_entry *lmm_find_module_by_type(lmm_type_t type) {
  struct lmm_entry *entry;
  list_for_each_entry(entry, &g_lmm_entries, list) {
    if (entry->type == type) {
      return entry;
    }
  }
  return nullptr;
}

size_t lmm_get_count(void) {
  return g_lmm_count;
}
