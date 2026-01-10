#pragma once

#include <aerosync/types.h>
#include <aerosync/sysintf/ic.h>

// PIT command for channel 2 (PC Speaker)
#define PIT_CMD_CHANNEL_2 0xB6
#define PIT_CHANNEL_2 0x42
#define PIT_COMMAND 0x43

int pic_probe(void); // will always return 1 - PIC is always present
int pic_install(void); // unconditionally sets up PIT
void pit_set_frequency(uint32_t hz); // with internal clamping
void pic_enable_irq(uint8_t irq_line);
void pic_disable_irq(uint8_t irq_line);
void pic_send_eoi(uint32_t interrupt_number);
void pic_mask_all(void);

const interrupt_controller_interface_t* pic_get_driver(void);