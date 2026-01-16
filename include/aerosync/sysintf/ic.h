#pragma once

#include <aerosync/types.h>

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
  fn(int, probe, void);
  fn(int, install, void);
  fn(int, init_ap, void);
  fn(void, timer_set, uint32_t frequency_hz);
  fn(void, timer_stop, void);
  fn(void, timer_oneshot, uint32_t microseconds);
  fn(void, timer_tsc_deadline, uint64_t deadline);
  fn(int, timer_has_tsc_deadline, void);
  fn(void, enable_irq, uint8_t irq_line);
  fn(void, disable_irq, uint8_t irq_line);
  fn(void, send_eoi, uint32_t interrupt_number);
  fn(void, mask_all, void);
  fn(void, shutdown, void);
  fn(void, send_ipi, uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);
  fn(uint8_t, get_id, void);
  uint32_t priority;
} interrupt_controller_interface_t;

#define IC_DEFAULT_TICK 100

// Unified interrupt controller interface
void ic_register_controller(const interrupt_controller_interface_t* controller);
interrupt_controller_t ic_install(void); // returns initialized controller type
void ic_ap_init(void);
void ic_shutdown_controller(void);
void ic_enable_irq(uint8_t irq_line);
void ic_disable_irq(uint8_t irq_line);
void ic_send_eoi(uint32_t interrupt_number);
void ic_set_timer(uint32_t frequency_hz);
uint32_t ic_get_frequency(void);
void ic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);
uint8_t ic_lapic_get_id(void);
void ic_register_lapic_get_id_early();

// Query functions
interrupt_controller_t ic_get_controller_type(void);
