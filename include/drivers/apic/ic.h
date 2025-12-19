#pragma once

#include <kernel/types.h>

typedef enum {
  INTC_PIC,
  INTC_APIC,
  INTC_OPIC,
  INTC_SAPIC,
  INTC_GIC,
  INTC_LPIC,
  INTC_UNKNOWN
} interrupt_controller_t;

typedef struct {
  interrupt_controller_t type;
  int (*probe)(void);
  int (*install)(void);
  void (*timer_set)(uint32_t frequency_hz);
  void (*enable_irq)(uint8_t irq_line);
  void (*disable_irq)(uint8_t irq_line);
  void (*send_eoi)(uint32_t interrupt_number);
  void (*mask_all)(void);
  void (*shutdown)(void);
  uint32_t priority;
} interrupt_controller_interface_t;

// Unified interrupt controller interface
void ic_register_controller(const interrupt_controller_interface_t* controller);
interrupt_controller_t ic_install(void); // returns initialized controller type
void ic_shutdown_controller(void);
void ic_enable_irq(uint8_t irq_line);
void ic_disable_irq(uint8_t irq_line);
void ic_send_eoi(uint32_t interrupt_number);
void ic_set_timer(uint32_t frequency_hz);
uint32_t ic_get_frequency(void);
void ic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);

// Query functions
interrupt_controller_t ic_get_controller_type(void);