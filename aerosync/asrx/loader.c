/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/asrx/loader.c
 * @brief Advanced AeroSync Runtime eXtension (ASRX) Loader
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/asrx.h>
#include <aerosync/mod_loader.h>
#include <aerosync/fkx/elf_parser.h>
#include <mm/vmalloc.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/limine_modules.h>
#include <mm/vma.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include "fkx_key.h"

static LIST_HEAD(asrx_modules);
static DEFINE_MUTEX(asrx_lock);

struct asrx_module *asrx_find_module(const char *name) {
  struct asrx_module *mod;
  list_for_each_entry(mod, &asrx_modules, list) {
    if (strcmp(mod->name, name) == 0) return mod;
  }
  return nullptr;
}

int asrx_load_from_memory(void *data, size_t size, const char *name_hint) {
  (void)name_hint;
  struct asrx_module *mod = kzalloc(sizeof(struct asrx_module));
  if (!mod) return -ENOMEM;

  struct mod_image *img = (struct mod_image *)kmalloc(sizeof(struct mod_image));
  if (!img) { kfree(mod); return -ENOMEM; }
  memset(img, 0, sizeof(struct mod_image));

  img->raw_data = data;
  img->raw_size = size;

  if (mod_verify_signature(data, size, g_fkx_root_key, FKX_KEY_SIZE) != 0) {
    kfree(img); kfree(mod); return -EPERM;
  }

  if (mod_map_segments(img) != 0) {
    kfree(img); kfree(mod); return -ENOEXEC;
  }

  const Elf64_Shdr *name_sec = elf_get_section(data, ".asrx_info");
  const Elf64_Shdr *lic_sec = elf_get_section(data, ".asrx_license");
  
  if (!name_sec || !lic_sec) {
    printk(KERN_ERR ASRX_CLASS "Module missing metadata sections\n");
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EINVAL;
  }

  strncpy(mod->name, (const char *)data + name_sec->sh_offset, ASRX_MODULE_NAME_LEN);
  img->license = *(uint32_t *)((uint8_t *)data + lic_sec->sh_offset);
  img->name = mod->name;
  mod->license = img->license;

  mutex_lock(&asrx_lock);
  if (asrx_find_module(mod->name)) {
    mutex_unlock(&asrx_lock);
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EEXIST;
  }

  if (mod_relocate(img) != 0) {
    mutex_unlock(&asrx_lock);
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EIO;
  }

  const Elf64_Shdr *init_sec = elf_get_section(data, ".asrx_init");
  const Elf64_Shdr *exit_sec = elf_get_section(data, ".asrx_exit");
  
  if (init_sec) mod->init = *(int (**)(void))((uint8_t *)img->base_addr + (init_sec->sh_addr - img->min_vaddr));
  if (exit_sec) mod->exit = *(void (**)(void))((uint8_t *)img->base_addr + (exit_sec->sh_addr - img->min_vaddr));

  if (mod_register_symbols(img) != 0) {
    mutex_unlock(&asrx_lock);
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EIO;
  }

  if (mod_apply_protections(img) != 0) {
    mutex_unlock(&asrx_lock);
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EIO;
  }

  if (mod->init && mod->init() != 0) {
    mutex_unlock(&asrx_lock);
    unregister_ksymbols_in_range((uintptr_t)img->base_addr, (uintptr_t)img->base_addr + img->total_size);
    mod_cleanup_image(img); kfree(img); kfree(mod); return -EBUSY;
  }

  mod->module_core = img->base_addr;
  mod->core_size = img->total_size;
  mod->state = ASRX_STATE_LIVE;
  
  list_add_tail(&mod->list, &asrx_modules);
  mutex_unlock(&asrx_lock);

  /* img structure itself can be freed as its data is now in asrx_module */
  kfree(img);

  printk(KERN_DEBUG ASRX_CLASS "Module '%s' loaded successfully\n", mod->name);
  return 0;
}

int asrx_load_from_file(const char *path) {
  struct file *file = vfs_open(path, O_RDONLY, 0);
  if (!file) return -ENOENT;

  size_t size = file->f_inode->i_size;
  void *buffer = vmalloc(size);
  if (!buffer) { vfs_close(file); return -ENOMEM; }

  vfs_loff_t pos = 0;
  if (kernel_read(file, buffer, size, &pos) != (ssize_t)size) {
    vfree(buffer); vfs_close(file); return -EIO;
  }

  int ret = asrx_load_from_memory(buffer, size, path);
  vfree(buffer);
  vfs_close(file);
  return ret;
}

int lmm_asrx_prober(const struct limine_file *file, uint32_t *out_type) {
  if (file->size < 128) return 0;
  if (!elf_verify(file->address, file->size)) return 0;

  if (mod_verify_signature(file->address, file->size, g_fkx_root_key, FKX_KEY_SIZE) != 0) return 0;

  if (elf_get_section(file->address, ".asrx_info") != nullptr) {
    *out_type = LMM_TYPE_ASRX;
    return 100;
  }
  return 0;
}

void __init lmm_load_asrx_callback(struct lmm_entry *entry, void *data) {
  (void)data;
  const struct limine_file *m = entry->file;
  asrx_load_from_memory(m->address, m->size, m->path);
}

int asrx_unload_module(const char *name) {
  mutex_lock(&asrx_lock);
  struct asrx_module *mod = asrx_find_module(name);
  if (!mod) { mutex_unlock(&asrx_lock); return -ENOENT; }

  if (atomic_read(&mod->refcnt) > 0) {
    mutex_unlock(&asrx_lock); return -EBUSY;
  }

  mod->state = ASRX_STATE_GOING;
  if (mod->exit) mod->exit();

  unregister_ksymbols_in_range((uintptr_t)mod->module_core, (uintptr_t)mod->module_core + mod->core_size);
  
  list_del(&mod->list);
  vfree(mod->module_core);
  kfree(mod);

  mutex_unlock(&asrx_lock);
  printk(KERN_DEBUG ASRX_CLASS "Module '%s' unloaded\n", name);
  return 0;
}
