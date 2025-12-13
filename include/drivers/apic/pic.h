#pragma once

#include <kernel/types.h>

void pic_install();
void pit_install();
void pit_set_frequency(uint16_t hz);
void pic_enable_irq(uint8_t irq_line);
void pic_disable_irq(uint8_t irq_line);
void pic_send_eoi(uint64_t interrupt_number);