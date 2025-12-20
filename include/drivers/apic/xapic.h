#pragma once

#include <kernel/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

// --- External Variables (defined in apic.c for global access) ---
extern volatile uint32_t *xapic_lapic_base;
extern volatile uint32_t *xapic_ioapic_base;
extern volatile uint32_t xapic_timer_hz;

extern uacpi_u64 xapic_madt_lapic_override_phys;   // 0 if not provided
extern uacpi_u32 xapic_madt_ioapic_phys;           // 0 if not provided
extern int xapic_madt_parsed;

#define XAPIC_MAX_IRQ_OVERRIDES 16
extern struct acpi_madt_interrupt_source_override xapic_irq_overrides[XAPIC_MAX_IRQ_OVERRIDES];
extern int xapic_num_irq_overrides;

// --- xAPIC Functions ---

// Initialize xAPIC
int xapic_setup_lapic(void);
int xapic_setup_ioapic(void);

// xAPIC I/O functions
void xapic_send_eoi(uint32_t irn);
void xapic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);
uint8_t xapic_get_id(void);

// xAPIC interrupt management
void xapic_enable_irq(uint8_t irq_line);
void xapic_disable_irq(uint8_t irq_line);
void xapic_mask_all(void);

// xAPIC timer functions
void xapic_timer_init(uint32_t frequency_hz);
void xapic_timer_set_frequency(uint32_t frequency_hz);

// xAPIC shutdown
void xapic_shutdown(void);

// xAPIC IPI Delivery Modes
#define XAPIC_DELIVERY_MODE_FIXED        (0b000 << 8)
#define XAPIC_DELIVERY_MODE_LOWEST_PRIO  (0b001 << 8)
#define XAPIC_DELIVERY_MODE_SMI          (0b010 << 8)
#define XAPIC_DELIVERY_MODE_NMI          (0b011 << 8)
#define XAPIC_DELIVERY_MODE_INIT         (0b100 << 8)
#define XAPIC_DELIVERY_MODE_STARTUP      (0b101 << 8)