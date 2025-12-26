#include <kernel/fkx/fkx.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <lib/printk.h>

extern const struct fkx_symbol _fkx_ksymtab_start[];
extern const struct fkx_symbol _fkx_ksymtab_end[];

struct fkx_dyn_symbol {
    struct fkx_symbol sym;
    struct fkx_dyn_symbol *next;
};

static struct fkx_dyn_symbol *g_dyn_symbols = NULL;

uintptr_t fkx_lookup_symbol(const char *name) {
    // 1. Search static kernel symbols
    const struct fkx_symbol *curr = _fkx_ksymtab_start;
    while (curr < _fkx_ksymtab_end) {
        if (strcmp(curr->name, name) == 0) {
            return curr->addr;
        }
        curr++;
    }

    // 2. Search dynamic module symbols
    struct fkx_dyn_symbol *dyn = g_dyn_symbols;
    while (dyn) {
        if (strcmp(dyn->sym.name, name) == 0) {
            return dyn->sym.addr;
        }
        dyn = dyn->next;
    }

    return 0;
}

int fkx_register_symbol(uintptr_t addr, const char *name) {
    if (!name) return -1;

    // Check if symbol already exists
    if (fkx_lookup_symbol(name)) {
        printk(KERN_WARNING "FKX: Symbol %s already registered, skipping\n", name);
        return -1;
    }

    struct fkx_dyn_symbol *new_sym = kmalloc(sizeof(struct fkx_dyn_symbol));
    if (!new_sym) return -1;

    new_sym->sym.addr = addr;
    new_sym->sym.name = name; // We assume the name string is persistent (e.g., in module's rodata)
    new_sym->next = g_dyn_symbols;
    g_dyn_symbols = new_sym;

    return 0;
}
