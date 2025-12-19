#pragma once

#include <kernel/types.h>


// Probe for APIC availability (CPUID feature bit). Returns non-zero if present.
int apic_probe(void);

// Main initialization function to detect and set up both Local APIC and I/O
// APIC. Returns non-zero on success, 0 on failure.
int apic_init(void);
int setup_lapic(void);
int setup_ioapic(void);

// Replaces PIC_enable_irq. Unmasks an interrupt line in the I/O APIC.
void apic_enable_irq(uint8_t irq_line);

// Replaces PIC_disable_irq. Masks an interrupt line in the I/O APIC.
void apic_disable_irq(uint8_t irq_line);

// Replaces PICMaskAll. Masks all interrupts at the I/O APIC level.
void apic_mask_all(void);

// Sends End-of-Interrupt signal to the Local APIC.
void apic_send_eoi(uint32_t irn); // irn arg for compatibility
void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);

// APIC IPI Delivery Modes
#define APIC_DELIVERY_MODE_FIXED        (0b000 << 8)
#define APIC_DELIVERY_MODE_LOWEST_PRIO  (0b001 << 8)
#define APIC_DELIVERY_MODE_SMI          (0b010 << 8)
#define APIC_DELIVERY_MODE_NMI          (0b011 << 8)
#define APIC_DELIVERY_MODE_INIT         (0b100 << 8)
#define APIC_DELIVERY_MODE_STARTUP      (0b101 << 8)

// Replaces PitInstall and PitSetFrequency.
// Initializes and starts the Local APIC timer at the specified frequency.
void apic_timer_init(uint32_t frequency_hz);

// Changes the APIC timer's frequency on the fly.
void apic_timer_set_frequency(uint32_t frequency_hz);
// Get the current CPU's LAPIC ID
uint8_t lapic_get_id(void);

#include <drivers/apic/ic.h>
const interrupt_controller_interface_t* apic_get_driver(void);
