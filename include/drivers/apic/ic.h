#pragma once

typedef enum {
  INTC_PIC = 0,
  INTC_APIC = 1,
} interrupt_controller_t;

// Unified interrupt controller interface
interrupt_controller_t ic_install(void); // returns initialized controller type
void ic_enable(void);
void ic_disable(void);
void ic_enable_irq(uint8_t irq_line);
void ic_disable_irq(uint8_t irq_line);
void ic_send_eoi(uint64_t interrupt_number);
void ic_set_timer(uint32_t frequency_hz);

// Query functions
interrupt_controller_t ic_get_controller_type(void);
const char* ic_get_controller_name(void);