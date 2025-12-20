#pragma once

#include <kernel/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

#define X2APIC_CLASS "[SYS::X2APIC] " // x2APIC configuration

// --- External Variables (defined in apic.c for global access) ---
extern volatile uint32_t *x2apic_ioapic_base;
extern volatile uint32_t x2apic_timer_hz;

extern uacpi_u64 x2apic_madt_lapic_override_phys;   // 0 if not provided
extern uacpi_u32 x2apic_madt_ioapic_phys;           // 0 if not provided
extern int x2apic_madt_parsed;

#define X2APIC_MAX_IRQ_OVERRIDES 16
extern struct acpi_madt_interrupt_source_override x2apic_irq_overrides[X2APIC_MAX_IRQ_OVERRIDES];
extern int x2apic_num_irq_overrides;

// --- x2APIC Functions ---

// Initialize x2APIC
int x2apic_setup_lapic(void);
int x2apic_setup_ioapic(void);

// x2APIC I/O functions
void x2apic_send_eoi(uint32_t irn);
void x2apic_send_ipi(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);
uint32_t x2apic_get_id(void);

// x2APIC interrupt management
void x2apic_enable_irq(uint8_t irq_line);
void x2apic_disable_irq(uint8_t irq_line);
void x2apic_mask_all(void);

// x2APIC timer functions
void x2apic_timer_init(uint32_t frequency_hz);
void x2apic_timer_set_frequency(uint32_t frequency_hz);

// x2APIC shutdown
void x2apic_shutdown(void);

// x2APIC IPI Delivery Modes
#define X2APIC_DELIVERY_MODE_FIXED        (0b000 << 8)
#define X2APIC_DELIVERY_MODE_LOWEST_PRIO  (0b001 << 8)
#define X2APIC_DELIVERY_MODE_SMI          (0b010 << 8)
#define X2APIC_DELIVERY_MODE_NMI          (0b011 << 8)
#define X2APIC_DELIVERY_MODE_INIT         (0b100 << 8)
#define X2APIC_DELIVERY_MODE_STARTUP      (0b101 << 8)