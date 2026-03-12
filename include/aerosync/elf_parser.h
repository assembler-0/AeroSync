#pragma once

#include <aerosync/elf.h>
#include <aerosync/types.h>

int elf_verify(void *data, size_t len);
const Elf64_Shdr *elf_get_section(void *data, const char *name);
const Elf64_Sym *elf_get_symbol(void *data, const char *name);
