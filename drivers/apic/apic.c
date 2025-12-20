#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <drivers/apic/apic.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <kernel/classes.h>
#include <lib/printk.h>
#include <drivers/apic/pic.h>
#include <drivers/apic/xapic.h>
#include <drivers/apic/x2apic.h>

// --- Global Variables ---
volatile uint32_t *xapic_lapic_base = NULL;
volatile uint32_t *xapic_ioapic_base = NULL;
volatile uint32_t xapic_timer_hz = 0;

uacpi_u64 xapic_madt_lapic_override_phys = 0;   // 0 if not provided
uacpi_u32 xapic_madt_ioapic_phys = 0;           // 0 if not provided
int xapic_madt_parsed = 0;

struct acpi_madt_interrupt_source_override xapic_irq_overrides[XAPIC_MAX_IRQ_OVERRIDES];
int xapic_num_irq_overrides = 0;

// x2APIC globals (shared with xAPIC for consistency)
volatile uint32_t *x2apic_ioapic_base = NULL;  // x2APIC reuses the same I/O APIC base
volatile uint32_t x2apic_timer_hz = 0;

uacpi_u64 x2apic_madt_lapic_override_phys = 0;   // 0 if not provided
uacpi_u32 x2apic_madt_ioapic_phys = 0;           // 0 if not provided
int x2apic_madt_parsed = 0;

struct acpi_madt_interrupt_source_override x2apic_irq_overrides[X2APIC_MAX_IRQ_OVERRIDES];
int x2apic_num_irq_overrides = 0;

// --- APIC Mode Selection ---
static int apic_mode = 0; // 0 = xAPIC, 1 = x2APIC

// --- Forward Declarations ---
static int detect_apic(void);
static void apic_parse_madt(void);

// --- APIC Mode Detection and Selection ---

static int detect_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 21)) != 0; // Check for x2APIC feature bit
}

static int detect_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0; // Check for APIC feature bit
}

// --- MADT parsing via uACPI ---
static uacpi_iteration_decision apic_madt_iter_cb(uacpi_handle user, struct acpi_entry_hdr* ehdr) {
    (void)user;
    switch (ehdr->type) {
        case ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE: {
            const struct acpi_madt_lapic_address_override* ovr = (const void*)ehdr;
            xapic_madt_lapic_override_phys = ovr->address;
            x2apic_madt_lapic_override_phys = ovr->address; // Shared between both modes
            break;
        }
        case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
            const struct acpi_madt_ioapic* io = (const void*)ehdr;
            if (xapic_madt_ioapic_phys == 0) {
                xapic_madt_ioapic_phys = io->address;
                x2apic_madt_ioapic_phys = io->address; // Shared between both modes
            }
            break;
        }
        case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
            const struct acpi_madt_interrupt_source_override* iso = (const void*)ehdr;
            if (xapic_num_irq_overrides < XAPIC_MAX_IRQ_OVERRIDES) {
                xapic_irq_overrides[xapic_num_irq_overrides] = *iso;
                x2apic_irq_overrides[xapic_num_irq_overrides] = *iso; // Shared between both modes
                xapic_num_irq_overrides++;
                x2apic_num_irq_overrides++;
            }
            break;
        }
        default:
            break;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void apic_parse_madt(void) {
    if (xapic_madt_parsed) return; // Use xapic flag as general MADT parsed flag

    uacpi_table tbl;
    uacpi_status st = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl);
    if (uacpi_likely_success(st)) {
        // Iterate subtables starting after struct acpi_madt header
        uacpi_for_each_subtable(tbl.hdr, sizeof(struct acpi_madt), apic_madt_iter_cb, NULL);
        uacpi_table_unref(&tbl);
        xapic_madt_parsed = 1;
        x2apic_madt_parsed = 1;
    } else {
        // Not fatal; fall back to defaults/MSR
        xapic_madt_parsed = 1;
        x2apic_madt_parsed = 1;
    }
}

// --- Core APIC Functions (Abstraction Layer) ---

int apic_init(void) {
    pic_mask_all();

    // Parse MADT via uACPI to obtain LAPIC/IOAPIC bases if provided
    apic_parse_madt();

    // Detect and select APIC mode
    if (detect_x2apic()) {
        printk(APIC_CLASS "x2APIC mode supported, attempting to enable\n");
        if (x2apic_setup_lapic()) {
            apic_mode = 1; // x2APIC mode
            printk(APIC_CLASS "x2APIC mode enabled\n");
        } else {
            printk(KERN_INFO APIC_CLASS "x2APIC mode failed, falling back to xAPIC\n");
            if (!xapic_setup_lapic()) {
                printk(KERN_ERR APIC_CLASS "Failed to setup Local APIC.\n");
                return false;
            }
        }
    } else {
        // x2APIC not supported, use xAPIC
        if (!xapic_setup_lapic()) {
            printk(KERN_ERR APIC_CLASS "Failed to setup Local APIC.\n");
            return false;
        }
    }

    // Initialize I/O APIC (same for both modes)
    if (apic_mode == 1) {
        if (!x2apic_setup_ioapic()) {
            printk(KERN_ERR APIC_CLASS "Failed to setup I/O APIC.\n");
            return false;
        }
    } else {
        if (!xapic_setup_ioapic()) {
            printk(KERN_ERR APIC_CLASS "Failed to setup I/O APIC.\n");
            return false;
        }
    }

    return true;
}

int apic_probe(void) {
    return detect_apic();
}

void apic_send_eoi(const uint32_t irn) { // arg for compatibility
    (void)irn;
    if (apic_mode == 1) {
        x2apic_send_eoi(irn);
    } else {
        xapic_send_eoi(irn);
    }
}

void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    if (apic_mode == 1) {
        // For x2APIC, we need to convert the 8-bit APIC ID to a 32-bit ID
        // In most cases, the 8-bit ID can be directly used as 32-bit ID for BSP
        x2apic_send_ipi((uint32_t)dest_apic_id, vector, delivery_mode);
    } else {
        xapic_send_ipi(dest_apic_id, vector, delivery_mode);
    }
}

uint8_t lapic_get_id(void) {
    if (apic_mode == 1) {
        // x2APIC returns 32-bit ID, but we only need the lower 8 bits for compatibility
        return (uint8_t)x2apic_get_id();
    }
    return xapic_get_id();
}

void apic_enable_irq(uint8_t irq_line) {
    if (apic_mode == 1) {
        x2apic_enable_irq(irq_line);
    } else {
        xapic_enable_irq(irq_line);
    }
}

void apic_disable_irq(uint8_t irq_line) {
    if (apic_mode == 1) {
        x2apic_disable_irq(irq_line);
    } else {
        xapic_disable_irq(irq_line);
    }
}

void apic_mask_all(void) {
    if (apic_mode == 1) {
        x2apic_mask_all();
    } else {
        xapic_mask_all();
    }
}

void apic_timer_init(uint32_t frequency_hz) {
    if (apic_mode == 1) {
        x2apic_timer_init(frequency_hz);
    } else {
        xapic_timer_init(frequency_hz);
    }
}

void apic_timer_set_frequency(uint32_t frequency_hz) {
    if (apic_mode == 1) {
        x2apic_timer_set_frequency(frequency_hz);
    } else {
        xapic_timer_set_frequency(frequency_hz);
    }
}

static void apic_shutdown(void) {
    if (apic_mode == 1) {
        x2apic_shutdown();
    } else {
        xapic_shutdown();
    }
    printk(APIC_CLASS "APIC shut down.\n");
}

static const interrupt_controller_interface_t apic_interface = {
    .type = INTC_APIC,
    .probe = apic_probe,
    .install = apic_init,
    .timer_set = apic_timer_init,
    .enable_irq = apic_enable_irq,
    .disable_irq = apic_disable_irq,
    .send_eoi = apic_send_eoi,
    .mask_all = apic_mask_all,
    .shutdown = apic_shutdown,
    .priority = 100,
};

const interrupt_controller_interface_t* apic_get_driver(void) {
    return &apic_interface;
}