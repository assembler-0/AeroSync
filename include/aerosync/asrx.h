/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/asrx.h
 * @brief AeroSync Runtime eXtension (ASRX) - Loadable Kernel Modules
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/ksymtab.h>
#include <aerosync/mutex.h>
#include <linux/list.h>

#define ASRX_MODULE_NAME_LEN 64

enum asrx_module_state {
  ASRX_STATE_LOADING,
  ASRX_STATE_LIVE,
  ASRX_STATE_GOING,
  ASRX_STATE_UNLOADED,
};

struct asrx_module {
  enum asrx_module_state state;
  struct list_head list;
  char name[ASRX_MODULE_NAME_LEN];

  /* Exported symbols from this module */
  struct ksymbol *syms;
  unsigned int num_syms;

  /* Symbols this module depends on (from other modules) */
  struct asrx_module_ref *deps;
  unsigned int num_deps;

  /* Initialization and Cleanup */
  int (*init)(void);
  void (*exit)(void);

  /* License of the module */
  uint32_t license; /* enum ksymbol_license */

  /* Memory regions */
  void *module_core;
  size_t core_size;
  void *module_init;
  size_t init_size;

  /* Reference counting for unloading */
  atomic_t refcnt;
};

struct asrx_module_ref {
  struct list_head list;
  struct asrx_module *target;
};

/* --- Module Macros --- */

#define asrx_module_init(initfn)			\
	static int (*__init_stub)(void) = initfn;\
	__attribute__((section(".asrx_init"), used)) \
	int (*__asrx_init)(void) = initfn\

#define asrx_module_exit(exitfn)			\
	static void (*__exit_stub)(void) = exitfn;\
	__attribute__((section(".asrx_exit"), used)) \
	void (*__asrx_exit)(void) = exitfn\

#define asrx_module_info(name, lic) \
    __attribute__((section(".asrx_info"), used))\
    const char __asrx_mod_name[] = #name; \
    __attribute__((section(".asrx_license"), used))\
    const uint32_t __asrx_mod_lic = lic\

/* --- Internal API --- */

struct limine_file;
struct lmm_entry;

/**
 * LMM Prober for ASRX modules
 */
int lmm_asrx_prober(const struct limine_file *file, uint32_t *out_type);

/**
 * LMM Callback for loading ASRX modules
 */
void lmm_load_asrx_callback(struct lmm_entry *entry, void *data);

/**
 * Load an ASRX module from a buffer in memory
 */
int asrx_load_from_memory(void *data, size_t size, const char *name_hint);

/**
 * Load an ASRX module from a file via VFS
 */
int asrx_load_from_file(const char *path);

int asrx_unload_module(const char *name);

struct asrx_module *asrx_find_module(const char *name);

/**
 * asrx_get_module - Increment module reference count
 */
bool asrx_get_module(struct asrx_module *mod);

/**
 * asrx_put_module - Decrement module reference count
 */
void asrx_put_module(struct asrx_module *mod);
