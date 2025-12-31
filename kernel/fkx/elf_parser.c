#include <kernel/elf.h>
#include <lib/string.h>
#include <kernel/types.h>

int elf_verify(void *data, size_t len) {
    if (!data || len < sizeof(Elf64_Ehdr)) return 0;
    
    Elf64_Ehdr *hdr = (Elf64_Ehdr *)data;
    
    if (hdr->e_ident[EI_MAG0] != ELFMAG0 ||
        hdr->e_ident[EI_MAG1] != ELFMAG1 ||
        hdr->e_ident[EI_MAG2] != ELFMAG2 ||
        hdr->e_ident[EI_MAG3] != ELFMAG3) {
        return 0;
    }
    
    if (hdr->e_ident[EI_CLASS] != 2) return 0; // Not 64-bit
    if (hdr->e_ident[EI_DATA] != 1) return 0;  // Not Little Endian
    if (hdr->e_machine != EM_X86_64) return 0; // Not x86_64
    
    return 1;
}

const Elf64_Shdr *elf_get_section(void *data, const char *name) {
    Elf64_Ehdr *hdr = (Elf64_Ehdr *)data;
    Elf64_Shdr *sections = (Elf64_Shdr *)((uint8_t *)data + hdr->e_shoff);
    
    // Validate string table index
    if (hdr->e_shstrndx == 0 || hdr->e_shstrndx >= hdr->e_shnum) return NULL;
    
    Elf64_Shdr *strtab_sec = &sections[hdr->e_shstrndx];
    const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
    
    for (int i = 0; i < hdr->e_shnum; i++) {
        const char *sec_name = strtab + sections[i].sh_name;
        if (strcmp(sec_name, name) == 0) {
            return &sections[i];
        }
    }
    
    return NULL;
}

const Elf64_Sym *elf_get_symbol(void *data, const char *name) {
    Elf64_Ehdr *hdr = (Elf64_Ehdr *)data;
    Elf64_Shdr *sections = (Elf64_Shdr *)((uint8_t *)data + hdr->e_shoff);
    
    const char *shstrtab = (const char *)((uint8_t *)data + sections[hdr->e_shstrndx].sh_offset);

    for (int i = 0; i < hdr->e_shnum; i++) {
        if (sections[i].sh_type == SHT_SYMTAB || sections[i].sh_type == SHT_DYNSYM) {
            Elf64_Sym *symtab = (Elf64_Sym *)((uint8_t *)data + sections[i].sh_offset);
            size_t sym_count = sections[i].sh_size / sizeof(Elf64_Sym);
            
            // Get associated string table
            if (sections[i].sh_link >= hdr->e_shnum) continue;
            Elf64_Shdr *strtab_sec = &sections[sections[i].sh_link];
            const char *strtab = (const char *)((uint8_t *)data + strtab_sec->sh_offset);
            
            for (size_t j = 0; j < sym_count; j++) {
                const char *sym_name = strtab + symtab[j].st_name;
                if (strcmp(sym_name, name) == 0) {
                    return &symtab[j];
                }
            }
        }
    }
    
    return NULL;
}
