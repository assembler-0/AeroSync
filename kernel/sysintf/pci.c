#include <kernel/sysintf/pci.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/classes.h>

#define MAX_PCI_OPS 4
static const pci_ops_t *registered_ops[MAX_PCI_OPS];
static int num_registered_ops = 0;
static const pci_ops_t *current_ops = NULL;

void pci_register_ops(const pci_ops_t *ops) {
    if (num_registered_ops >= MAX_PCI_OPS) return;
    registered_ops[num_registered_ops++] = ops;

    if (!current_ops || ops->priority > current_ops->priority) {
        if (ops->probe && ops->probe() == 0) {
            current_ops = ops;
            printk(KERN_INFO PCI_CLASS "Switched to %s ops\n", ops->name);
        }
    }
}

uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
    if (current_ops) return current_ops->read(p, offset, width);
    return 0xFFFFFFFF;
}

void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
    if (current_ops) current_ops->write(p, offset, val, width);
}

// Export the symbols so modules can use them
#include <kernel/fkx/fkx.h>
EXPORT_SYMBOL(pci_register_ops);
EXPORT_SYMBOL(pci_read);
EXPORT_SYMBOL(pci_write);

