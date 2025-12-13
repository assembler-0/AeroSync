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

// Replaces PICSendEOI. Sends End-of-Interrupt signal to the Local APIC.
void apic_send_eoi(uint32_t irn); // irn arg for compatibility

// Replaces PitInstall and PitSetFrequency.
// Initializes and starts the Local APIC timer at the specified frequency.
void apic_timer_init(uint32_t frequency_hz);

// Changes the APIC timer's frequency on the fly.
void apic_timer_set_frequency(uint32_t frequency_hz);

void pic_mask_all(void);

// Get the current CPU's LAPIC ID
uint8_t lapic_get_id(void);
