#include <aerosync/fkx/fkx.h>
#include <aerosync/fkx/elf_parser.h>
#include <mm/slab.h>
#include <mm/vmalloc.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/classes.h>

#define FKX_DEBUG 1

// Structure to represent a loaded module image
struct fkx_loaded_image {
  struct fkx_loaded_image *next;      /* Next image in the list */
  struct fkx_module_info *info;       /* Module info pointer */
  void *base_addr;                    /* Base address where module is loaded */
  size_t size;                        /* Size of the loaded module */
  fkx_module_class_t module_class;    /* Class of the module */
  uint32_t flags;                     /* Module flags */
  int linked;                         /* Whether relocations have been applied */
  int initialized;                    /* Whether the module has been initialized */

  // Stored for the relocation phase
  void *raw_data;
  uint64_t min_vaddr;
};

// Array to hold heads of linked lists for each module class (linked modules)
static struct fkx_loaded_image *g_module_class_heads[FKX_MAX_CLASS] = {NULL};

// List of modules that are mapped but not yet linked
static struct fkx_loaded_image *g_unlinked_modules = NULL;

static int fkx_relocate_module(struct fkx_loaded_image *img);

int fkx_load_image(void *data, size_t size) {
  if (!elf_verify(data, size)) {
    printk(KERN_ERR FKX_CLASS "Invalid ELF magic or architecture\n");
    return -1;
  }

  Elf64_Ehdr *hdr = (Elf64_Ehdr *) data;

  // We only support ET_DYN (Shared Object) for now
  if (hdr->e_type != ET_DYN) {
    printk(KERN_ERR FKX_CLASS "Module must be ET_DYN (PIE/Shared Object)\n");
    return -1;
  }

  // 1. Calculate memory requirements
  uint64_t min_vaddr = (uint64_t) -1;
  uint64_t max_vaddr = 0;

  Elf64_Phdr *phdrs = (Elf64_Phdr *) ((uint8_t *) data + hdr->e_phoff);

  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_LOAD) {
      if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
      if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
    }
  }

  size_t total_size = max_vaddr - min_vaddr;
  if (total_size == 0) {
    printk(KERN_ERR FKX_CLASS "No loadable segments found\n");
    return -1;
  }

  // 2. Allocate memory
  void *base = vmalloc_exec(total_size);
  if (!base) {
    printk(KERN_ERR FKX_CLASS "Failed to allocate memory for module\n");
    return -1;
  }

  uint64_t base_addr = (uint64_t) base;

  // 3. Load segments
  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_LOAD) {
      void *dest = (void *) (base_addr + (phdrs[i].p_vaddr - min_vaddr));
      void *src = (void *) ((uint8_t *) data + phdrs[i].p_offset);

      // Copy file content
      if (phdrs[i].p_filesz > 0) {
        memcpy(dest, src, phdrs[i].p_filesz);
      }

      // Zero out BSS
      if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
        memset((void *) ((uint64_t) dest + phdrs[i].p_filesz), 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
      }
    }
  }

  // 4. Find Module Info (BEFORE relocation)
  const Elf64_Shdr *info_sec = elf_get_section(data, ".fkx_info");
  if (!info_sec) {
    printk(KERN_ERR FKX_CLASS ".fkx_info section not found\n");
    vfree(base);
    return -1;
  }

  struct fkx_module_info *info = NULL;

  if (info_sec->sh_flags & SHF_ALLOC) {
    info = (struct fkx_module_info *) (base_addr + (info_sec->sh_addr - min_vaddr));
  } else {
    info = (struct fkx_module_info *) ((uint8_t *) data + info_sec->sh_offset);
  }

  if (info->magic != FKX_MAGIC) {
    printk(KERN_ERR FKX_CLASS "Invalid module magic: %x\n", info->magic);
    vfree(base);
    return -1;
  }

  // 5. Create loaded image structure and add to unlinked list
  struct fkx_loaded_image *loaded_img = (struct fkx_loaded_image *)kmalloc(sizeof(struct fkx_loaded_image));
  if (!loaded_img) {
    printk(KERN_ERR FKX_CLASS "Failed to allocate memory for loaded image structure\n");
    vfree(base);
    return -1;
  }

  loaded_img->info = info;
  loaded_img->base_addr = base;
  loaded_img->size = total_size;
  loaded_img->module_class = info->module_class;
  loaded_img->flags = info->flags;
  loaded_img->linked = 0;
  loaded_img->initialized = 0;
  loaded_img->raw_data = data;
  loaded_img->min_vaddr = min_vaddr;

  // Add to unlinked modules list
  loaded_img->next = g_unlinked_modules;
  g_unlinked_modules = loaded_img;

  return 0;
}

static int fkx_relocate_module(struct fkx_loaded_image *img) {
  void *data = img->raw_data;
  uint64_t base_addr = (uint64_t) img->base_addr;
  uint64_t min_vaddr = img->min_vaddr;

  Elf64_Ehdr *hdr = (Elf64_Ehdr *) data;
  Elf64_Shdr *sections = (Elf64_Shdr *) ((uint8_t *) data + hdr->e_shoff);

  for (int i = 0; i < hdr->e_shnum; i++) {
    if (sections[i].sh_type == SHT_RELA) {
      Elf64_Rela *relas = (Elf64_Rela *) ((uint8_t *) data + sections[i].sh_offset);
      size_t count = sections[i].sh_size / sizeof(Elf64_Rela);

      Elf64_Shdr *symtab_sec = &sections[sections[i].sh_link];
      Elf64_Sym *symtab = (Elf64_Sym *) ((uint8_t *) data + symtab_sec->sh_offset);

      for (size_t j = 0; j < count; j++) {
        uint64_t r_offset = relas[j].r_offset;
        uint64_t *target = (uint64_t *) (base_addr + (r_offset - min_vaddr));

        uint64_t type = ELF64_R_TYPE(relas[j].r_info);
        uint32_t sym_idx = ELF64_R_SYM(relas[j].r_info);
        int64_t addend = relas[j].r_addend;

        Elf64_Sym *sym = &symtab[sym_idx];
        uint64_t S = 0;

        if (sym->st_shndx != 0) {
          S = base_addr + (sym->st_value - min_vaddr);
        } else {
          const char *sym_name = "?";
          if (symtab_sec->sh_link != 0) {
              Elf64_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
              const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
              sym_name = strtab + sym->st_name;
          }
          S = fkx_lookup_symbol(sym_name);
        }

        switch (type) {
          case R_X86_64_RELATIVE:
            *target = base_addr + addend;
            break;

          case R_X86_64_64:
            if (S == 0 && sym->st_shndx == SHN_UNDEF) {
              const char *sym_name = "?";
              if (symtab_sec->sh_link != 0) {
                  Elf64_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
                  const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
                  sym_name = strtab + sym->st_name;
              }
              printk(KERN_ERR FKX_CLASS "Undefined symbol '%s' in R_X86_64_64 relocation\n", sym_name);
              return -1;
            }
            *target = S + addend;
            break;

          case R_X86_64_JUMP_SLOT:
          case R_X86_64_GLOB_DAT:
            if (S == 0 && sym->st_shndx == SHN_UNDEF) {
              const char *sym_name = "?";
              if (symtab_sec->sh_link != 0) {
                  Elf64_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
                  const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
                  sym_name = strtab + sym->st_name;
              }
              printk(KERN_ERR FKX_CLASS "Undefined symbol '%s' in PLT/GOT relocation\n", sym_name);
              return -1;
            }
            *target = S;
            break;

          case R_X86_64_PC32:
          case R_X86_64_PLT32:
          {
            uint64_t P = (uint64_t) target;
            int32_t value = (int32_t) ((S + addend) - P);
            *(int32_t *) target = value;
          }
          break;

          default:
            printk(KERN_WARNING FKX_CLASS "Unhandled relocation type %lu at offset 0x%lx\n",
                   type, r_offset);
            break;
        }
      }
    }
  }

  // Register module symbols
  const Elf64_Shdr *ksymtab_sec = elf_get_section(data, "fkx_ksymtab");
  if (ksymtab_sec) {
      struct fkx_symbol *syms = (struct fkx_symbol *)(base_addr + (ksymtab_sec->sh_addr - min_vaddr));
      size_t count = ksymtab_sec->sh_size / sizeof(struct fkx_symbol);
      
      for (size_t i = 0; i < count; i++) {
          fkx_register_symbol(syms[i].addr, syms[i].name);
      }
  }

  img->linked = 1;
  return 0;
}

static struct fkx_loaded_image *find_unlinked_by_name(const char *name) {
  struct fkx_loaded_image *curr = g_unlinked_modules;
  while (curr) {
    if (strcmp(curr->info->name, name) == 0) return curr;
    curr = curr->next;
  }
  return NULL;
}

static struct fkx_loaded_image *find_linked_by_name(const char *name) {
  for (int i = 0; i < FKX_MAX_CLASS; i++) {
    struct fkx_loaded_image *curr = g_module_class_heads[i];
    while (curr) {
      if (strcmp(curr->info->name, name) == 0) return curr;
      curr = curr->next;
    }
  }
  return NULL;
}

int fkx_finalize_loading(void) {
  int total_to_link = 0;
  struct fkx_loaded_image *curr = g_unlinked_modules;
  while (curr) {
    total_to_link++;
    curr = curr->next;
  }

  if (total_to_link == 0) return 0;

  printk(KERN_DEBUG FKX_CLASS "Finalizing loading for %d modules...\n", total_to_link);

  int linked_in_this_pass;
  do {
    linked_in_this_pass = 0;
    struct fkx_loaded_image *prev = NULL;
    curr = g_unlinked_modules;

    while (curr) {
      int deps_satisfied = 1;
      if (curr->info->depends) {
        for (int i = 0; curr->info->depends[i] != NULL; i++) {
          const char *dep_name = curr->info->depends[i];
          if (!find_linked_by_name(dep_name)) {
            // Dependency not yet linked. Check if it's even in our unlinked list.
            if (!find_unlinked_by_name(dep_name)) {
              printk(KERN_ERR FKX_CLASS "Module '%s' depends on '%s', which is NOT found!\n",
                     curr->info->name, dep_name);
              // This module can never be satisfied.
              deps_satisfied = 0;
              break; 
            }
            deps_satisfied = 0;
            break;
          }
        }
      }

      if (deps_satisfied) {
        if (fkx_relocate_module(curr) == 0) {
          printk(KERN_DEBUG FKX_CLASS "Linked module '%s'\n", curr->info->name);
          
          // Remove from unlinked list
          struct fkx_loaded_image *to_link = curr;
          if (prev) prev->next = curr->next;
          else g_unlinked_modules = curr->next;
          
          curr = curr->next;

          // Add to class list
          fkx_module_class_t class = to_link->module_class;
          to_link->next = g_module_class_heads[class];
          g_module_class_heads[class] = to_link;

          linked_in_this_pass++;
        } else {
          printk(KERN_ERR FKX_CLASS "Failed to link module '%s'\n", curr->info->name);
          // For now, just skip it and let it stay in unlinked list (or we could remove it)
          prev = curr;
          curr = curr->next;
        }
      } else {
        prev = curr;
        curr = curr->next;
      }
    }
  } while (linked_in_this_pass > 0);

  // Check if any modules remain unlinked
  if (g_unlinked_modules) {
    curr = g_unlinked_modules;
    while (curr) {
      printk(KERN_ERR FKX_CLASS "Module '%s' could not be linked (circular dependency or missing dependency)\n",
             curr->info->name);
      curr = curr->next;
    }
    return -1;
  }

  return 0;
}

int fkx_init_module_class(fkx_module_class_t module_class) {
  if (module_class >= FKX_MAX_CLASS) {
    printk(KERN_ERR FKX_CLASS "Invalid module class: %d\n", module_class);
    return -1;
  }

  struct fkx_loaded_image *current_mod = g_module_class_heads[module_class];
  int count = 0;

  // First pass: count total modules in this class
  struct fkx_loaded_image *temp = current_mod;
  while (temp) {
    count++;
    temp = temp->next;
  }

  if (count == 0) {
    return 0;
  }

  printk(KERN_DEBUG FKX_CLASS "Initializing %d modules in class %d\n", count, module_class);

  // Second pass: initialize all modules in this class
  current_mod = g_module_class_heads[module_class];
  int initialized_count = 0;
  int error_count = 0;

  while (current_mod) {
    if (!current_mod->initialized && current_mod->info->init) {
      printk(KERN_DEBUG FKX_CLASS "Initializing module '%s' in class %d\n", current_mod->info->name, module_class);

      int ret = current_mod->info->init();
      if (ret != 0) {
        printk(KERN_ERR FKX_CLASS "Module '%s' init failed: %d\n", current_mod->info->name, ret);
        error_count++;
        // Continue with other modules even if one fails
      } else {
        current_mod->initialized = 1;
        initialized_count++;
      }
    }
    current_mod = current_mod->next;
  }

  printk(KERN_DEBUG FKX_CLASS "%d/%d modules in class %d initialized successfully\n",
         initialized_count, count, module_class);

  return (error_count == 0) ? 0 : -1;
}