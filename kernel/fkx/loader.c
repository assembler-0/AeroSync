#include <kernel/fkx/fkx.h>
#include <kernel/fkx/elf_parser.h>
#include <mm/slab.h>
#include <mm/vmalloc.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/io.h>
#include <drivers/timer/time.h>
#include <lib/ic.h>
#include <kernel/classes.h>
#include <kernel/panic.h>

#define FKX_DEBUG 1

extern volatile struct limine_framebuffer_request *get_framebuffer_request(void);

// Structure to represent a loaded module image
struct fkx_loaded_image {
  struct fkx_loaded_image *next;      /* Next image in the list */
  struct fkx_module_info *info;       /* Module info pointer */
  void *base_addr;                    /* Base address where module is loaded */
  size_t size;                        /* Size of the loaded module */
  fkx_module_class_t module_class;    /* Class of the module */
  uint32_t flags;                     /* Module flags */
  int initialized;                    /* Whether the module has been initialized */
};

// Array to hold heads of linked lists for each module class
static struct fkx_loaded_image *g_module_class_heads[FKX_MAX_CLASS] = {NULL};

// Global API Table
static struct fkx_kernel_api g_fkx_api = {
  .version = FKX_API_VERSION,
  .reserved = 0,
  .kmalloc = kmalloc,
  .kfree = kfree,
  .vmalloc = vmalloc,
  .vmalloc_exec = vmalloc_exec,
  .vfree = vfree,
  .viomap = viomap,
  .viounmap = viounmap,
  .vmm_map_page = vmm_map_page,
  .vmm_unmap_page = vmm_unmap_page,
  .vmm_virt_to_phys = vmm_virt_to_phys,
  .vmm_switch_pml4 = vmm_switch_pml4,
  .memset = memset,
  .memcpy = memcpy,
  .memmove = memmove,
  .memcmp = memcmp,
  .strlen = strlen,
  .strcpy = strcpy,
  .strcmp = strcmp,
  .printk = printk,
  .snprintf = snprintf,
  .panic = panic,
  .pmm_alloc_page = pmm_alloc_page,
  .pmm_free_page = pmm_free_page,
  .pmm_alloc_pages = pmm_alloc_pages,
  .pmm_free_pages = pmm_free_pages,
  .pmm_phys_to_virt = pmm_phys_to_virt,
  .pmm_virt_to_phys = pmm_virt_to_phys,
  .inb = inb,
  .inw = inw,
  .inl = inl,
  .outb = outb,
  .outw = outw,
  .outl = outl,
  .wrmsr = wrmsr,
  .rdmsr = rdmsr,
  .save_irq_flags = save_irq_flags,
  .restore_irq_flags = restore_irq_flags,
  .cpuid = cpuid,
  .cpuid_count = cpuid_count,
  .ndelay = time_wait_ns,
  .udelay = delay_us,
  .mdelay = delay_ms,
  .sdelay = delay_s,
  .get_time_ns = get_time_ns,
  .rdtsc = rdtsc,
  .time_register_source = time_register_source,
  .ic_register_controller = ic_register_controller,
  .ic_shutdown_controller = ic_shutdown_controller,
  .ic_enable_irq = ic_enable_irq,
  .ic_disable_irq = ic_disable_irq,
  .ic_send_eoi = ic_send_eoi,
  .ic_set_timer = ic_set_timer,
  .ic_get_frequency = ic_get_frequency,
  .ic_send_ipi = ic_send_ipi,
  .ic_get_controller_type = ic_get_controller_type,
  .get_framebuffer_request = get_framebuffer_request,
  .printk_register_backend = printk_register_backend,
  .printk_set_sink = printk_set_sink,
  .printk_shutdown = printk_shutdown,
  .spinlock_init = spinlock_init,
  .spinlock_lock = spinlock_lock,
  .spinlock_unlock = spinlock_unlock,
  .spinlock_lock_irqsave = spinlock_lock_irqsave,
  .spinlock_unlock_irqrestore = spinlock_unlock_irqrestore,
  .mutex_init = mutex_init,
  .mutex_lock = mutex_lock,
  .mutex_unlock = mutex_unlock,
  .mutex_trylock = mutex_trylock,
  .mutex_is_locked = mutex_is_locked,
  .uacpi_table_find_by_signature = uacpi_table_find_by_signature,
  .uacpi_for_each_subtable = uacpi_for_each_subtable,
  .uacpi_table_unref = uacpi_table_unref
};

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

  // 4. Apply Relocations
  Elf64_Shdr *sections = (Elf64_Shdr *) ((uint8_t *) data + hdr->e_shoff);

  for (int i = 0; i < hdr->e_shnum; i++) {
    if (sections[i].sh_type == SHT_RELA) {
      Elf64_Rela *relas = (Elf64_Rela *) ((uint8_t *) data + sections[i].sh_offset);
      size_t count = sections[i].sh_size / sizeof(Elf64_Rela);

      // The symbol table used for relocations
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
          // SHN_UNDEF is 0
          S = base_addr + (sym->st_value - min_vaddr);
        }

        switch (type) {
          case R_X86_64_RELATIVE:
            // B + A (base + addend)
            *target = base_addr + addend;
            break;

          case R_X86_64_64:
            // S + A
            if (sym->st_shndx == SHN_UNDEF) {
              const char *sym_name = "?";
              if (symtab_sec->sh_link != 0) {
                  Elf64_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
                  const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
                  sym_name = strtab + sym->st_name;
              }
              printk(KERN_ERR FKX_CLASS "Undefined symbol '%s' in R_X86_64_64 relocation\n", sym_name);
              vfree(base);
              return -1;
            }
            *target = S + addend;
            break;

          case R_X86_64_JUMP_SLOT:
          case R_X86_64_GLOB_DAT:
            // S (for GOT/PLT entries)
            if (sym->st_shndx == SHN_UNDEF) {
              const char *sym_name = "?";
              if (symtab_sec->sh_link != 0) {
                  Elf64_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
                  const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
                  sym_name = strtab + sym->st_name;
              }
              printk(KERN_ERR FKX_CLASS "Undefined symbol '%s' in PLT/GOT relocation\n", sym_name);
              vfree(base);
              return -1;
            }
            *target = S;
            break;

          case R_X86_64_PC32: // You might encounter these too
          case R_X86_64_PLT32:
            // S + A - P (symbol + addend - place)
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

  // 5. Find Module Info
  const Elf64_Shdr *info_sec = elf_get_section(data, ".fkx_info");
  if (!info_sec) {
    printk(KERN_ERR FKX_CLASS ".fkx_info section not found\n");
    vfree(base);
    return -1;
  }

  struct fkx_module_info *info = NULL;

  if (info_sec->sh_flags & SHF_ALLOC) {
    if (!(info_sec->sh_flags & SHF_ALLOC)) {
      printk(KERN_ERR FKX_CLASS ".fkx_info section must be allocated (SHF_ALLOC)\n");
      vfree(base);
      return -1;
    }

    info = (struct fkx_module_info *) (base_addr + (info_sec->sh_addr - min_vaddr));
  } else {
    // It's not in a loaded segment? That's weird for .fkx_info, but let's support reading from raw data
    info = (struct fkx_module_info *) ((uint8_t *) data + info_sec->sh_offset);
  }

  if (info->magic != FKX_MAGIC) {
    printk(KERN_ERR FKX_CLASS "Invalid module magic: %x\n", info->magic);
    vfree(base);
    return -1;
  }

  printk(FKX_CLASS "Loaded image for module '%s' v%s by %s class %d\n", info->name, info->version, info->author, info->module_class);

  // 6. Create loaded image structure and add to appropriate class list
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
  loaded_img->initialized = 0;

  // Add to the beginning of the appropriate class list
  fkx_module_class_t class = info->module_class;
  if (class >= FKX_MAX_CLASS) {
    printk(KERN_ERR FKX_CLASS "Invalid module class: %d\n", class);
    kfree(loaded_img);
    vfree(base);
    return -1;
  }

  loaded_img->next = g_module_class_heads[class];
  g_module_class_heads[class] = loaded_img;

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
    printk(KERN_DEBUG FKX_CLASS "No modules found for class %d\n", module_class);
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

      int ret = current_mod->info->init(&g_fkx_api);
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
