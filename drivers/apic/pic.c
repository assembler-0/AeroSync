///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/pic.c
 * @brief Legacy PIC interrupt controller driver
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/classes.h> 
#include <kernel/fkx/fkx.h>
#include <kernel/sysintf/ic.h>
#include <arch/x64/io.h>
#include <drivers/timer/pit.h>
#include <lib/printk.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10
#define ICW4_8086 0x01

static uint16_t s_irq_mask = 0xFFFF; // All masked initially

static void pic_write_mask() {
  outb(PIC1_DATA, s_irq_mask & 0xFF);
  outb(PIC2_DATA, (s_irq_mask >> 8) & 0xFF);
}

void pic_mask_all(void) {
  s_irq_mask = 0xFFFF;
  pic_write_mask();
}

void pic_enable_irq(uint8_t irq_line) {
  if (irq_line > 15)
    return;
  s_irq_mask &= ~(1 << irq_line);
  pic_write_mask();
}

void pic_disable_irq(uint8_t irq_line) {
  if (irq_line > 15)
    return;
  s_irq_mask |= (1 << irq_line);
  pic_write_mask();
}

void pic_send_eoi(uint32_t interrupt_number) {
  if (interrupt_number >= 40)
    outb(PIC2_COMMAND, 0x20);
  outb(PIC1_COMMAND, 0x20);
}

int pic_install(void) {
  printk(KERN_NOTICE PIC_CLASS "PIC drivers does not come with builtin PIT timer\n");
  
  outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
  outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
  outb(PIC1_DATA, 0x20); 
  outb(PIC2_DATA, 0x28); 
  outb(PIC1_DATA, 4); 
  outb(PIC2_DATA, 2); 
  outb(PIC1_DATA, ICW4_8086);
  outb(PIC2_DATA, ICW4_8086);

  return 1;
}

int pic_probe(void) {
  return 1; 
}

static void pic_shutdown(void) {
    pic_mask_all();
    printk(PIC_CLASS "PIC shut down.\n");
}

static const interrupt_controller_interface_t pic_interface = {
    .type = INTC_PIC,
    .probe = pic_probe,
    .install = pic_install,
    .timer_set = pit_set_frequency,
    .enable_irq = pic_enable_irq,
    .disable_irq = pic_disable_irq,
    .send_eoi = pic_send_eoi,
    .mask_all = pic_mask_all,
    .shutdown = pic_shutdown,
    .priority = 50,
};

const interrupt_controller_interface_t* pic_get_driver(void) {
    return &pic_interface;
}
