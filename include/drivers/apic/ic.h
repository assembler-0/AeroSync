#pragma once

#include <kernel/types.h>

typedef enum {
  INTC_PIC = 0,
  INTC_APIC = 1,
} interrupt_controller_t;

typedef struct {
  interrupt_controller_t type;
  int (*probe)(void);
  int (*install)(void);
  void (*timer_set)(uint32_t frequency_hz);
  void (*enable_irq)(uint8_t irq_line);
  void (*disable_irq)(uint8_t irq_line);
  void (*send_eoi)(uint32_t interrupt_number);
  uint32_t priority;
} interrupt_controller_interface_t;

// Unified interrupt controller interface
interrupt_controller_t ic_install(void); // returns initialized controller type
void ic_enable_irq(uint8_t irq_line);
void ic_disable_irq(uint8_t irq_line);
void ic_send_eoi(uint32_t interrupt_number);
void ic_set_timer(uint32_t frequency_hz);
uint32_t ic_get_frequency(void);

// Query functions
interrupt_controller_t ic_get_controller_type(void);
const char* ic_get_controller_name(void);