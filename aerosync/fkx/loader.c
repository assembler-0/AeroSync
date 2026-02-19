/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/fkx/loader.c
 * @brief FKX (Fused Kernel eXtension) loader
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/fkx/fkx.h>
#include <aerosync/fkx/elf_parser.h>
#include <aerosync/mod_loader.h>
#include <mm/slub.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/classes.h>
#include <aerosync/limine_modules.h>
#include <aerosync/errno.h>
#include "fkx_key.h"

struct fkx_loaded_image {
  struct fkx_loaded_image *next; 
  struct fkx_module_info *info; 
  struct mod_image mod;
  fkx_module_class_t module_class; 
  uint32_t flags; 
  int linked; 
  int initialized; 
};

static struct fkx_loaded_image *g_module_class_heads[FKX_MAX_CLASS] = {nullptr};
static struct fkx_loaded_image *g_unlinked_modules = nullptr;
static uint64_t g_available_subclasses = 0;
static uint64_t g_initialized_subclasses = 0;

int lmm_fkx_prober(const struct limine_file *file, lmm_type_t *out_type) {
  if (file->size < 128) return 0; // Minimal size for ELF + Signature
  if (!elf_verify(file->address, file->size)) return 0;

  if (mod_verify_signature(file->address, file->size, g_fkx_root_key, FKX_KEY_SIZE) != 0) return 0;

  if (elf_get_section(file->address, ".fkx_info") != nullptr) {
    *out_type = LMM_TYPE_FKX;
    return 100;
  }
  return 0;
}

void __init lmm_load_fkx_callback(struct lmm_entry *entry, void *data) {
  (void)data;
  const struct limine_file *m = entry->file;
  if (fkx_load_image(m->address, m->size) == 0) {
    printk(KERN_DEBUG FKX_CLASS "Loaded module: %s\n", m->path);
  }
}

int fkx_load_image(void *data, size_t size) {
  struct fkx_loaded_image *loaded_img = kzalloc(sizeof(struct fkx_loaded_image));
  if (!loaded_img) return -ENOMEM;

  loaded_img->mod.raw_data = data;
  loaded_img->mod.raw_size = size;

  if (mod_verify_signature(data, size, g_fkx_root_key, FKX_KEY_SIZE) != 0) {
    kfree(loaded_img); return -EPERM;
  }

  if (mod_map_segments(&loaded_img->mod) != 0) {
    kfree(loaded_img); return -ENOEXEC;
  }

  const Elf64_Shdr *info_sec = elf_get_section(data, ".fkx_info");
  if (!info_sec) {
    mod_cleanup_image(&loaded_img->mod); kfree(loaded_img); return -ENOENT;
  }

  struct fkx_module_info *info = (info_sec->sh_flags & SHF_ALLOC) ? 
      (struct fkx_module_info *)((uint64_t)loaded_img->mod.base_addr + (info_sec->sh_addr - loaded_img->mod.min_vaddr)) :
      (struct fkx_module_info *)((uint8_t *)data + info_sec->sh_offset);

  if (info->magic != FKX_MAGIC) {
    mod_cleanup_image(&loaded_img->mod); kfree(loaded_img); return -EINVAL;
  }

  loaded_img->info = info;
  loaded_img->mod.license = info->license;
  loaded_img->mod.name = info->name;
  loaded_img->module_class = info->module_class;
  loaded_img->flags = info->flags;

  loaded_img->next = g_unlinked_modules;
  g_unlinked_modules = loaded_img;

  return 0;
}

static int fkx_relocate_module(struct fkx_loaded_image *img) {
  if (mod_relocate(&img->mod) != 0) return -EIO;
  if (mod_register_symbols(&img->mod) != 0) return -EIO;
  if (mod_apply_protections(&img->mod) != 0) return -EIO;

  img->linked = 1;
  return 0;
}

int fkx_finalize_loading(void) {
  int linked_in_this_pass;
  do {
    linked_in_this_pass = 0;
    struct fkx_loaded_image *prev = nullptr, *curr = g_unlinked_modules;
    while (curr) {
      if ((curr->info->requirements & g_available_subclasses) == curr->info->requirements) {
        if (fkx_relocate_module(curr) == 0) {
          g_available_subclasses |= curr->info->subclass;
          if (prev) prev->next = curr->next;
          else g_unlinked_modules = curr->next;
          struct fkx_loaded_image *next = curr->next;
          curr->next = g_module_class_heads[curr->module_class];
          g_module_class_heads[curr->module_class] = curr;
          curr = next; linked_in_this_pass++;
          continue;
        }
      }
      prev = curr; curr = curr->next;
    }
  } while (linked_in_this_pass > 0);
  
  if (g_unlinked_modules) {
    struct fkx_loaded_image *curr = g_unlinked_modules;
    while (curr) {
      printk(KERN_ERR FKX_CLASS "Failed to link module '%s' (requirements: 0x%lx, available: 0x%lx)\n",
             curr->info->name, curr->info->requirements, g_available_subclasses);
      curr = curr->next;
    }
    return -ENODEV;
  }
  return 0;
}

int __no_cfi fkx_init_module_class(fkx_module_class_t module_class) {
  if (module_class >= FKX_MAX_CLASS) return -EINVAL;
  int initialized_this_pass;
  do {
    initialized_this_pass = 0;
    struct fkx_loaded_image *m = g_module_class_heads[module_class];
    while (m) {
      if (!m->initialized && (m->info->requirements & g_initialized_subclasses) == m->info->requirements) {
        int ret = m->info->init ? m->info->init() : 0;
        if (ret == 0) {
          m->initialized = 1;
          g_initialized_subclasses |= m->info->subclass;
          initialized_this_pass++;
        } else {
          m->initialized = -1;
        }
      }
      m = m->next;
    }
  } while (initialized_this_pass > 0);
  return 0;
}
