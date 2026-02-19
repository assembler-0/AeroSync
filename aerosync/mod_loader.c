/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/mod_loader.c
 * @brief Common Module Loader Utilities Implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/mod_loader.h>
#include <aerosync/fkx/elf_parser.h>
#include <aerosync/crypto.h>
#include <aerosync/errno.h>
#include <mm/vmalloc.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/mm/paging.h>
#include <mm/vma.h>

#define SIG_MAGIC 0x21474953 // 'SIG!'

struct mod_signature_footer {
  uint8_t signature[64];
  uint32_t magic;
};

int mod_verify_signature(void *data, size_t size, const uint8_t *key, size_t key_size) {
  if (size < sizeof(struct mod_signature_footer)) return -EINVAL;

  size_t data_size = size - sizeof(struct mod_signature_footer);
  struct mod_signature_footer *footer = (struct mod_signature_footer *)((uint8_t *)data + data_size);

  if (footer->magic != SIG_MAGIC) return -EPERM;

  uint8_t calculated_mac[64];
  crypto_hmac("sha512", key, key_size, data, data_size, calculated_mac);

  if (memcmp(calculated_mac, footer->signature, 64) != 0) return -EPERM;

  return 0;
}

int mod_map_segments(struct mod_image *img) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)img->raw_data;
  uint64_t min_vaddr = (uint64_t)-1, max_vaddr = 0;

  Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)img->raw_data + hdr->e_phoff);
  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_LOAD) {
      if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
      if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
    }
  }

  if (max_vaddr == 0) return -ENOEXEC;

  img->min_vaddr = min_vaddr;
  img->total_size = max_vaddr - min_vaddr;
  img->base_addr = vmalloc_exec(img->total_size);
  if (!img->base_addr) return -ENOMEM;

  img->load_bias = (uint64_t)img->base_addr - min_vaddr;

  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_LOAD) {
      void *dest = (uint8_t *)img->base_addr + (phdrs[i].p_vaddr - min_vaddr);
      if (phdrs[i].p_filesz > 0) {
        memcpy(dest, (uint8_t *)img->raw_data + phdrs[i].p_offset, phdrs[i].p_filesz);
      }
      if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
        memset((uint8_t *)dest + phdrs[i].p_filesz, 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
      }
    }
  }

  return 0;
}

int mod_relocate(struct mod_image *img) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)img->raw_data;
  Elf64_Shdr *sections = (Elf64_Shdr *)((uint8_t *)img->raw_data + hdr->e_shoff);

  for (int i = 0; i < hdr->e_shnum; i++) {
    if (sections[i].sh_type == SHT_RELA) {
      Elf64_Rela *relas = (Elf64_Rela *)((uint8_t *)img->raw_data + sections[i].sh_offset);
      size_t count = sections[i].sh_size / sizeof(Elf64_Rela);
      Elf64_Sym *symtab = (Elf64_Sym *)((uint8_t *)img->raw_data + sections[sections[i].sh_link].sh_offset);
      const char *strtab = (const char *)((uint8_t *)img->raw_data + sections[sections[sections[i].sh_link].sh_link].sh_offset);

      for (size_t j = 0; j < count; j++) {
        uint64_t *target = (uint64_t *)(img->load_bias + relas[j].r_offset);
        uint32_t type = ELF64_R_TYPE(relas[j].r_info);
        uint32_t sym_idx = ELF64_R_SYM(relas[j].r_info);
        uint64_t S = 0;

        if (type == R_X86_64_NONE) continue;
        if (type == R_X86_64_RELATIVE) {
          *target = img->load_bias + relas[j].r_addend;
          continue;
        }

        Elf64_Sym *sym = &symtab[sym_idx];
        if (sym->st_shndx != SHN_UNDEF) {
          S = img->load_bias + sym->st_value;
        } else {
          const char *name = strtab + sym->st_name;
          if (name[0] == '\0') {
            S = img->load_bias + sym->st_value;
          } else {
            S = lookup_ksymbol_licensed(name, img->license);
            if (S == 0 && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
              return -ENOENT;
            }
          }
        }

        switch (type) {
          case R_X86_64_64: *target = S + relas[j].r_addend; break;
          case R_X86_64_GLOB_DAT:
          case R_X86_64_JUMP_SLOT: *target = S; break;
          case R_X86_64_32: *(uint32_t *)target = (uint32_t)(S + relas[j].r_addend); break;
          case R_X86_64_32S: *(int32_t *)target = (int32_t)(S + relas[j].r_addend); break;
          case R_X86_64_PC32:
          case R_X86_64_PLT32:
          case R_X86_64_GOTPCREL: {
            uint64_t P = (uint64_t)target;
            *(int32_t *)target = (int32_t)(S + relas[j].r_addend - P);
          } break;
          default: return -ENOSYS;
        }
      }
    }
  }
  return 0;
}

int mod_register_symbols(struct mod_image *img) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)img->raw_data;
  Elf64_Shdr *sections = (Elf64_Shdr *)((uint8_t *)img->raw_data + hdr->e_shoff);

  /* 1. Register exports from ksymtab */
  const Elf64_Shdr *ksym_sec = elf_get_section(img->raw_data, "ksymtab");
  if (ksym_sec) {
    struct ksymbol *syms = (struct ksymbol *)(img->load_bias + ksym_sec->sh_addr);
    size_t count = ksym_sec->sh_size / sizeof(struct ksymbol);
    for (size_t i = 0; i < count; i++) {
      register_ksymbol(syms[i].addr, syms[i].name, img->license);
    }
  }

  /* 2. Register all functions for stack traces */
  for (int i = 0; i < hdr->e_shnum; i++) {
    if (sections[i].sh_type == SHT_SYMTAB) {
      Elf64_Sym *symtab = (Elf64_Sym *)((uint8_t *)img->raw_data + sections[i].sh_offset);
      size_t count = sections[i].sh_size / sizeof(Elf64_Sym);
      const char *strtab = (const char *)((uint8_t *)img->raw_data + sections[sections[i].sh_link].sh_offset);

      for (size_t j = 0; j < count; j++) {
        uint8_t type = ELF64_ST_TYPE(symtab[j].st_info);
        if ((type == STT_FUNC || type == STT_OBJECT) && 
            symtab[j].st_shndx != SHN_UNDEF && symtab[j].st_name != 0) {
          register_ksymbol(img->load_bias + symtab[j].st_value, strtab + symtab[j].st_name, img->license);
        }
      }
    }
  }
  return 0;
}

static int set_memory_prot(void *addr, size_t size, uint64_t flags) {
  uint64_t start = (uint64_t)addr & PAGE_MASK;
  uint64_t end = PAGE_ALIGN_UP((uint64_t)addr + size);
  
  for (uint64_t curr = start; curr < end; curr += PAGE_SIZE) {
    int ret = vmm_set_flags(&init_mm, curr, flags | PTE_PRESENT | PTE_GLOBAL);
    if (ret < 0) return ret;
  }
  return 0;
}

int mod_apply_protections(struct mod_image *img) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)img->raw_data;
  Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)img->raw_data + hdr->e_phoff);

  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_LOAD) {
      uint64_t prot = PTE_NX;
      if (phdrs[i].p_flags & PF_X) prot = 0;
      if (phdrs[i].p_flags & PF_W) prot |= PTE_RW;
      set_memory_prot((uint8_t *)img->base_addr + (phdrs[i].p_vaddr - img->min_vaddr), phdrs[i].p_memsz, prot);
    }
  }
  return 0;
}

void mod_cleanup_image(struct mod_image *img) {
  if (img->base_addr) {
    vfree(img->base_addr);
    img->base_addr = nullptr;
  }
}
