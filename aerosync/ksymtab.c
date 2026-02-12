/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/ksymtab.c
 * @brief Kernel symbol table helpers
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <aerosync/classes.h>
#include <aerosync/ksymtab.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <mm/vmalloc.h>
#include <lib/printk.h>
#include <aerosync/elf.h>
#include <aerosync/spinlock.h>

extern const struct ksymbol _ksymtab_start[];
extern const struct ksymbol _ksymtab_end[];

struct dyn_ksymbol {
  struct ksymbol sym;
  struct dyn_ksymbol *next;
};

static struct dyn_ksymbol *g_dyn_symbols = nullptr;
static DEFINE_SPINLOCK(g_dyn_symbols_lock);

// Full kernel ELF symbol table support
static Elf64_Sym *kernel_symtab = nullptr;
static size_t kernel_symtab_count = 0;
static const char *kernel_strtab = nullptr;
static uintptr_t kernel_slide = 0;

// Optimized Index
struct ksym_idx_entry {
  uintptr_t addr;
  uint32_t name_offset; // Offset into kernel_strtab
  uint32_t size;
};

static struct ksym_idx_entry *ksym_index = nullptr;
static size_t ksym_index_count = 0;

void ksymtab_init(void *kernel_base_addr) {
  if (!kernel_base_addr) return;

  Elf64_Ehdr *hdr = (Elf64_Ehdr *) kernel_base_addr;

  // Basic ELF verification
  if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1 ||
      hdr->e_ident[EI_MAG2] != ELFMAG2 || hdr->e_ident[EI_MAG3] != ELFMAG3) {
    printk(KERN_WARNING KERN_CLASS "ksymtab: Invalid ELF magic\n");
    return;
  }

  Elf64_Shdr *sections = (Elf64_Shdr *) ((uint8_t *) kernel_base_addr + hdr->e_shoff);

  for (int i = 0; i < hdr->e_shnum; i++) {
    if (sections[i].sh_type == SHT_SYMTAB) {
      kernel_symtab = (Elf64_Sym *) ((uint8_t *) kernel_base_addr + sections[i].sh_offset);
      kernel_symtab_count = sections[i].sh_size / sizeof(Elf64_Sym);

      if (sections[i].sh_link < hdr->e_shnum) {
        Elf64_Shdr *strtab_sec = &sections[sections[i].sh_link];
        kernel_strtab = (const char *) ((uint8_t *) kernel_base_addr + strtab_sec->sh_offset);
      }
      break;
    }
  }

  if (kernel_symtab && kernel_strtab) {
    printk(KERN_INFO KERN_CLASS "ksymtab: loaded %lu symbols from kernel ELF (early)\n", kernel_symtab_count);

    // Calculate KASLR slide
    // We look for the symbol "ksymtab_init" which corresponds to this function.
    // Its st_value is the link-time address.
    // The runtime address is (uintptr_t)&ksymtab_init.

    for (size_t i = 0; i < kernel_symtab_count; i++) {
      if (kernel_symtab[i].st_name != 0) {
        const char *name = kernel_strtab + kernel_symtab[i].st_name;
        if (strcmp(name, "ksymtab_init") == 0) {
          kernel_slide = (uintptr_t) &ksymtab_init - kernel_symtab[i].st_value;
          printk(KERN_INFO KERN_CLASS "ksymtab: detected KASLR slide: %p (Link: %p, Run: %p)\n",
                 (void *) kernel_slide, (void *) kernel_symtab[i].st_value, (void *) &ksymtab_init);
          break;
        }
      }
    }
  } else {
    printk(KERN_WARNING KERN_CLASS "ksymtab: failed to find symbol table in kernel ELF\n");
  }
}

// ShellSort implementation for ksym_idx_entry
static void ksym_sort(struct ksym_idx_entry *arr, size_t n) {
  for (size_t gap = n / 2; gap > 0; gap /= 2) {
    for (size_t i = gap; i < n; i += 1) {
      struct ksym_idx_entry temp = arr[i];
      size_t j;
      for (j = i; j >= gap && arr[j - gap].addr > temp.addr; j -= gap) {
        arr[j] = arr[j - gap];
      }
      arr[j] = temp;
    }
  }
}

void ksymtab_finalize(void) {
  if (!kernel_symtab || !kernel_strtab) return;
  if (ksym_index) return; // Already finalized

  // 1. Count valid function symbols
  size_t valid_count = 0;
  for (size_t i = 0; i < kernel_symtab_count; i++) {
    unsigned char type = ELF64_ST_TYPE(kernel_symtab[i].st_info);
    if ((type == STT_FUNC || type == STT_OBJECT) && kernel_symtab[i].st_value != 0 && kernel_symtab[i].st_shndx !=
        SHN_UNDEF) {
      valid_count++;
    }
  }

  // 2. Allocate index
  ksym_index = vmalloc(valid_count * sizeof(struct ksym_idx_entry));
  if (!ksym_index) {
    printk(KERN_ERR KERN_CLASS "ksymtab: failed to allocate index\n");
    return;
  }

  // 3. Populate index
  size_t idx = 0;
  for (size_t i = 0; i < kernel_symtab_count; i++) {
    unsigned char type = ELF64_ST_TYPE(kernel_symtab[i].st_info);
    if ((type == STT_FUNC || type == STT_OBJECT) && kernel_symtab[i].st_value != 0 && kernel_symtab[i].st_shndx !=
        SHN_UNDEF) {
      ksym_index[idx].addr = kernel_symtab[i].st_value + kernel_slide;
      ksym_index[idx].name_offset = kernel_symtab[i].st_name;
      ksym_index[idx].size = kernel_symtab[i].st_size;
      idx++;
    }
  }
  ksym_index_count = idx;

  // 4. Sort index
  ksym_sort(ksym_index, ksym_index_count);

  printk(KERN_INFO KERN_CLASS "ksymtab: built optimized index with %lu symbols\n", ksym_index_count);
}

uintptr_t lookup_ksymbol(const char *name) {
  // 1. Search static kernel symbols (Exported)
  const struct ksymbol *curr = _ksymtab_start;
  while (curr < _ksymtab_end) {
    if (strcmp(curr->name, name) == 0) {
      return curr->addr;
    }
    curr++;
  }

  // 2. Search dynamic module symbols
  irq_flags_t flags = spinlock_lock_irqsave(&g_dyn_symbols_lock);
  struct dyn_ksymbol *dyn = g_dyn_symbols;
  while (dyn) {
    if (strcmp(dyn->sym.name, name) == 0) {
      uintptr_t addr = dyn->sym.addr;
      spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);
      return addr;
    }
    dyn = dyn->next;
  }
  spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);

  return 0;
}

const char *lookup_ksymbol_by_addr(uintptr_t addr, uintptr_t *offset) {
  // 1. Try optimized index first (Binary Search)
  if (ksym_index) {
    long low = 0;
    long high = ksym_index_count - 1;
    long best = -1;

    while (low <= high) {
      long mid = low + (high - low) / 2;
      if (ksym_index[mid].addr <= addr) {
        best = mid;
        low = mid + 1;
      } else {
        high = mid - 1;
      }
    }

    if (best != -1) {
      uintptr_t sym_addr = ksym_index[best].addr;
      uintptr_t diff = addr - sym_addr;
      if (offset) *offset = diff;
      return kernel_strtab + ksym_index[best].name_offset;
    }
  }

  // 2. Search dynamic module symbols (Linear)
  irq_flags_t flags = spinlock_lock_irqsave(&g_dyn_symbols_lock);
  struct dyn_ksymbol *dyn = g_dyn_symbols;
  const char *best_name = nullptr;
  uintptr_t best_addr = 0;

  while (dyn) {
    if (dyn->sym.addr <= addr) {
      if (!best_name || dyn->sym.addr > best_addr) {
        best_name = dyn->sym.name;
        best_addr = dyn->sym.addr;
      }
    }
    dyn = dyn->next;
  }

  if (best_name) {
    if (offset) *offset = addr - best_addr;
    spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);
    return best_name;
  }
  spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);

  // 3. Fallback: Search full kernel symbol table (Linear) if index not built yet (Panic early)
  if (!ksym_index && kernel_symtab && kernel_strtab) {
    for (size_t i = 0; i < kernel_symtab_count; i++) {
      unsigned char type = ELF64_ST_TYPE(kernel_symtab[i].st_info);
      if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
      if (kernel_symtab[i].st_value == 0 || kernel_symtab[i].st_shndx == SHN_UNDEF) continue;

      uintptr_t sym_addr = kernel_symtab[i].st_value + kernel_slide;
      if (sym_addr <= addr) {
        if (!best_name || sym_addr > best_addr) {
          best_name = kernel_strtab + kernel_symtab[i].st_name;
          best_addr = sym_addr;
        }
      }
    }
    if (best_name) {
      if (offset) *offset = addr - best_addr;
      return best_name;
    }
  }

  return nullptr;
}

int register_ksymbol(uintptr_t addr, const char *name) {
  if (!name) return -EINVAL;

  struct dyn_ksymbol *new_sym = kmalloc(sizeof(struct dyn_ksymbol));
  if (!new_sym) return -ENOMEM;

  new_sym->sym.addr = addr;
  new_sym->sym.name = name;

  irq_flags_t flags = spinlock_lock_irqsave(&g_dyn_symbols_lock);
  new_sym->next = g_dyn_symbols;
  g_dyn_symbols = new_sym;
  spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);

  return 0;
}

int unregister_ksymbol(uintptr_t addr) {
  irq_flags_t flags = spinlock_lock_irqsave(&g_dyn_symbols_lock);
  struct dyn_ksymbol *curr = g_dyn_symbols;
  struct dyn_ksymbol *prev = nullptr;

  while (curr) {
    if (curr->sym.addr == addr) {
      if (prev) {
        prev->next = curr->next;
      } else {
        g_dyn_symbols = curr->next;
      }
      spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);
      kfree(curr);
      return 0;
    }
    prev = curr;
    curr = curr->next;
  }

  spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);
  return -ENOENT;
}

void unregister_ksymbols_in_range(uintptr_t start_addr, uintptr_t end_addr) {
  irq_flags_t flags = spinlock_lock_irqsave(&g_dyn_symbols_lock);
  struct dyn_ksymbol *curr = g_dyn_symbols;
  struct dyn_ksymbol *prev = nullptr;

  while (curr) {
    if (curr->sym.addr >= start_addr && curr->sym.addr < end_addr) {
      struct dyn_ksymbol *to_free = curr;
      if (prev) {
        prev->next = curr->next;
      } else {
        g_dyn_symbols = curr->next;
      }
      curr = curr->next;
      kfree(to_free);
      continue;
    }
    prev = curr;
    curr = curr->next;
  }
  spinlock_unlock_irqrestore(&g_dyn_symbols_lock, flags);
}
