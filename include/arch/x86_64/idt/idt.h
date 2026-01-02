#pragma once

#include <kernel/types.h>
#include <compiler.h>

// An entry in the IDT (64-bit)
struct IdtEntry {
    uint16_t BaseLow;
    uint16_t Selector;
    uint8_t  Reserved;
    uint8_t  Flags;
    uint16_t BaseHigh;
    uint32_t BaseUpper;
    uint32_t Reserved2;
} __packed;

// A pointer to the IDT
struct IdtPtr {
    uint16_t Limit;
    uint64_t Base;
} __packed;


int idt_install();
void idt_load(struct IdtPtr* idtPtr);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

extern struct IdtPtr g_IdtPtr;
