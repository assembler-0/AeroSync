/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/idt/idt.c
 * @brief Interrupt Descriptor Table (IDT) setup and installation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <aerosync/classes.h>
#include <lib/printk.h>
#include <arch/x86_64/idt/idt.h>

#define IDT_ENTRIES 256

struct IdtEntry g_Idt[IDT_ENTRIES];
struct IdtPtr   g_IdtPtr;

// Declare all ISRs from Interrupts.asm
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();
extern void isr32();
extern void isr33();
extern void isr34();
extern void isr35();
extern void isr36();
extern void isr37();
extern void isr38();
extern void isr39();
extern void isr40();
extern void isr41();
extern void isr42();
extern void isr43();
extern void isr44();
extern void isr45();
extern void isr46();
extern void isr47();
extern void isr48();
extern void isr49();
extern void isr50();
extern void isr51();
extern void isr52();
extern void isr53();
extern void isr54();
extern void isr55();
extern void isr56();
extern void isr57();
extern void isr58();
extern void isr59();
extern void isr60();
extern void isr61();
extern void isr62();
extern void isr63();
extern void isr64();
extern void isr65();
extern void isr66();
extern void isr67();
extern void isr68();
extern void isr69();
extern void isr70();
extern void isr71();
extern void isr72();
extern void isr73();
extern void isr74();
extern void isr75();
extern void isr76();
extern void isr77();
extern void isr78();
extern void isr79();
extern void isr80();
extern void isr81();
extern void isr82();
extern void isr83();
extern void isr84();
extern void isr85();
extern void isr86();
extern void isr87();
extern void isr88();
extern void isr89();
extern void isr90();
extern void isr91();
extern void isr92();
extern void isr93();
extern void isr94();
extern void isr95();
extern void isr96();
extern void isr97();
extern void isr98();
extern void isr99();
extern void isr100();
extern void isr101();
extern void isr102();
extern void isr103();
extern void isr104();
extern void isr105();
extern void isr106();
extern void isr107();
extern void isr108();
extern void isr109();
extern void isr110();
extern void isr111();
extern void isr112();
extern void isr113();
extern void isr114();
extern void isr115();
extern void isr116();
extern void isr117();
extern void isr118();
extern void isr119();
extern void isr120();
extern void isr121();
extern void isr122();
extern void isr123();
extern void isr124();
extern void isr125();
extern void isr126();
extern void isr127();
extern void isr128();
extern void isr129();
extern void isr130();
extern void isr131();
extern void isr132();
extern void isr133();
extern void isr134();
extern void isr135();
extern void isr136();
extern void isr137();
extern void isr138();
extern void isr139();
extern void isr140();
extern void isr141();
extern void isr142();
extern void isr143();
extern void isr144();
extern void isr145();
extern void isr146();
extern void isr147();
extern void isr148();
extern void isr149();
extern void isr150();
extern void isr151();
extern void isr152();
extern void isr153();
extern void isr154();
extern void isr155();
extern void isr156();
extern void isr157();
extern void isr158();
extern void isr159();
extern void isr160();
extern void isr161();
extern void isr162();
extern void isr163();
extern void isr164();
extern void isr165();
extern void isr166();
extern void isr167();
extern void isr168();
extern void isr169();
extern void isr170();
extern void isr171();
extern void isr172();
extern void isr173();
extern void isr174();
extern void isr175();
extern void isr176();
extern void isr177();
extern void isr178();
extern void isr179();
extern void isr180();
extern void isr181();
extern void isr182();
extern void isr183();
extern void isr184();
extern void isr185();
extern void isr186();
extern void isr187();
extern void isr188();
extern void isr189();
extern void isr190();
extern void isr191();
extern void isr192();
extern void isr193();
extern void isr194();
extern void isr195();
extern void isr196();
extern void isr197();
extern void isr198();
extern void isr199();
extern void isr200();
extern void isr201();
extern void isr202();
extern void isr203();
extern void isr204();
extern void isr205();
extern void isr206();
extern void isr207();
extern void isr208();
extern void isr209();
extern void isr210();
extern void isr211();
extern void isr212();
extern void isr213();
extern void isr214();
extern void isr215();
extern void isr216();
extern void isr217();
extern void isr218();
extern void isr219();
extern void isr220();
extern void isr221();
extern void isr222();
extern void isr223();
extern void isr224();
extern void isr225();
extern void isr226();
extern void isr227();
extern void isr228();
extern void isr229();
extern void isr230();
extern void isr231();
extern void isr232();
extern void isr233();
extern void isr234();
extern void isr235();
extern void isr236();
extern void isr237();
extern void isr238();
extern void isr239();
extern void isr240();
extern void isr241();
extern void isr242();
extern void isr243();
extern void isr244();
extern void isr245();
extern void isr246();
extern void isr247();
extern void isr248();
extern void isr249();
extern void isr250();
extern void isr251();
extern void isr252();
extern void isr253();
extern void isr254();
extern void isr255();

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    g_Idt[num].BaseLow = (base & 0xFFFF);
    g_Idt[num].Selector = sel;
    g_Idt[num].Reserved = ist & 0x07;
    g_Idt[num].Flags = flags;
    g_Idt[num].BaseHigh = (base >> 16) & 0xFFFF;
    g_Idt[num].BaseUpper = (base >> 32) & 0xFFFFFFFF;
    g_Idt[num].Reserved2 = 0;
}

int idt_install() {
    printk(IDT_CLASS "Installing IDT\n");
    g_IdtPtr.Limit = (sizeof(struct IdtEntry) * IDT_ENTRIES) - 1;
    g_IdtPtr.Base  = (uint64_t)&g_Idt;

    // The code segment selector is 0x08 (from our GDT)
    uint16_t kcode_segment = 0x08;
    uint8_t flags = 0x8E; // Present, DPL 0, 64-bit Interrupt Gate
    uint8_t flags_user = 0xEE; // Present, DPL 3, 64-bit Interrupt Gate

    // Exceptions (0-31) - These should be accessible from user mode (ring 3) when they occur due to user code
    idt_set_gate(0, (uint64_t)isr0, kcode_segment, flags_user, 0);  // Divide by Zero
    idt_set_gate(1, (uint64_t)isr1, kcode_segment, flags_user, 0);  // Debug
    idt_set_gate(2, (uint64_t)isr2, kcode_segment, flags_user, 1);  // NMI (IST 1)
    idt_set_gate(3, (uint64_t)isr3, kcode_segment, flags_user, 0);  // Breakpoint
    idt_set_gate(4, (uint64_t)isr4, kcode_segment, flags_user, 0);  // Overflow
    idt_set_gate(5, (uint64_t)isr5, kcode_segment, flags_user, 0);  // Bound Range Exceeded
    idt_set_gate(6, (uint64_t)isr6, kcode_segment, flags_user, 0);  // Invalid Opcode
    idt_set_gate(7, (uint64_t)isr7, kcode_segment, flags_user, 0);  // Device Not Available
    idt_set_gate(8, (uint64_t)isr8, kcode_segment, flags_user, 1);  // Double Fault (IST 1)
    idt_set_gate(9, (uint64_t)isr9, kcode_segment, flags_user, 0);  // Coprocessor Segment Overrun
    idt_set_gate(10, (uint64_t)isr10, kcode_segment, flags_user, 0); // Invalid TSS
    idt_set_gate(11, (uint64_t)isr11, kcode_segment, flags_user, 0); // Segment Not Present
    idt_set_gate(12, (uint64_t)isr12, kcode_segment, flags_user, 1); // Stack Fault (IST 1)
    idt_set_gate(13, (uint64_t)isr13, kcode_segment, flags_user, 1); // General Protection Fault (IST 1)
    idt_set_gate(14, (uint64_t)isr14, kcode_segment, flags_user, 0); // Page Fault
    idt_set_gate(15, (uint64_t)isr15, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(16, (uint64_t)isr16, kcode_segment, flags_user, 0); // x87 FPU Floating-Point exception
    idt_set_gate(17, (uint64_t)isr17, kcode_segment, flags_user, 0); // Alignment Check
    idt_set_gate(18, (uint64_t)isr18, kcode_segment, flags_user, 1); // Machine Check (IST 1)
    idt_set_gate(19, (uint64_t)isr19, kcode_segment, flags_user, 0); // SIMD Floating-Point exception
    idt_set_gate(20, (uint64_t)isr20, kcode_segment, flags_user, 0); // Virtualization exception
    idt_set_gate(21, (uint64_t)isr21, kcode_segment, flags_user, 0); // Control protocol exception
    idt_set_gate(22, (uint64_t)isr22, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(23, (uint64_t)isr23, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(24, (uint64_t)isr24, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(25, (uint64_t)isr25, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(26, (uint64_t)isr26, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(27, (uint64_t)isr27, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(28, (uint64_t)isr28, kcode_segment, flags_user, 0); // Hypervisor injection exception
    idt_set_gate(29, (uint64_t)isr29, kcode_segment, flags_user, 0); // VMM communication exception
    idt_set_gate(30, (uint64_t)isr30, kcode_segment, flags_user, 0); // Security exception
    idt_set_gate(31, (uint64_t)isr31, kcode_segment, flags_user, 0); // Reserved
    idt_set_gate(32, (uint64_t)isr32, kcode_segment, flags, 0);
    idt_set_gate(33, (uint64_t)isr33, kcode_segment, flags, 0);
    idt_set_gate(34, (uint64_t)isr34, kcode_segment, flags, 0);
    idt_set_gate(35, (uint64_t)isr35, kcode_segment, flags, 0);
    idt_set_gate(36, (uint64_t)isr36, kcode_segment, flags, 0);
    idt_set_gate(37, (uint64_t)isr37, kcode_segment, flags, 0);
    idt_set_gate(38, (uint64_t)isr38, kcode_segment, flags, 0);
    idt_set_gate(39, (uint64_t)isr39, kcode_segment, flags, 0);
    idt_set_gate(40, (uint64_t)isr40, kcode_segment, flags, 0);
    idt_set_gate(41, (uint64_t)isr41, kcode_segment, flags, 0);
    idt_set_gate(42, (uint64_t)isr42, kcode_segment, flags, 0);
    idt_set_gate(43, (uint64_t)isr43, kcode_segment, flags, 0);
    idt_set_gate(44, (uint64_t)isr44, kcode_segment, flags, 0);
    idt_set_gate(45, (uint64_t)isr45, kcode_segment, flags, 0);
    idt_set_gate(46, (uint64_t)isr46, kcode_segment, flags, 0);
    idt_set_gate(47, (uint64_t)isr47, kcode_segment, flags, 0);
    idt_set_gate(48, (uint64_t)isr48, kcode_segment, flags, 0);
    idt_set_gate(49, (uint64_t)isr49, kcode_segment, flags, 0);
    idt_set_gate(50, (uint64_t)isr50, kcode_segment, flags, 0);
    idt_set_gate(51, (uint64_t)isr51, kcode_segment, flags, 0);
    idt_set_gate(52, (uint64_t)isr52, kcode_segment, flags, 0);
    idt_set_gate(53, (uint64_t)isr53, kcode_segment, flags, 0);
    idt_set_gate(54, (uint64_t)isr54, kcode_segment, flags, 0);
    idt_set_gate(55, (uint64_t)isr55, kcode_segment, flags, 0);
    idt_set_gate(56, (uint64_t)isr56, kcode_segment, flags, 0);
    idt_set_gate(57, (uint64_t)isr57, kcode_segment, flags, 0);
    idt_set_gate(58, (uint64_t)isr58, kcode_segment, flags, 0);
    idt_set_gate(59, (uint64_t)isr59, kcode_segment, flags, 0);
    idt_set_gate(60, (uint64_t)isr60, kcode_segment, flags, 0);
    idt_set_gate(61, (uint64_t)isr61, kcode_segment, flags, 0);
    idt_set_gate(62, (uint64_t)isr62, kcode_segment, flags, 0);
    idt_set_gate(63, (uint64_t)isr63, kcode_segment, flags, 0);
    idt_set_gate(64, (uint64_t)isr64, kcode_segment, flags, 0);
    idt_set_gate(65, (uint64_t)isr65, kcode_segment, flags, 0);
    idt_set_gate(66, (uint64_t)isr66, kcode_segment, flags, 0);
    idt_set_gate(67, (uint64_t)isr67, kcode_segment, flags, 0);
    idt_set_gate(68, (uint64_t)isr68, kcode_segment, flags, 0);
    idt_set_gate(69, (uint64_t)isr69, kcode_segment, flags, 0);
    idt_set_gate(70, (uint64_t)isr70, kcode_segment, flags, 0);
    idt_set_gate(71, (uint64_t)isr71, kcode_segment, flags, 0);
    idt_set_gate(72, (uint64_t)isr72, kcode_segment, flags, 0);
    idt_set_gate(73, (uint64_t)isr73, kcode_segment, flags, 0);
    idt_set_gate(74, (uint64_t)isr74, kcode_segment, flags, 0);
    idt_set_gate(75, (uint64_t)isr75, kcode_segment, flags, 0);
    idt_set_gate(76, (uint64_t)isr76, kcode_segment, flags, 0);
    idt_set_gate(77, (uint64_t)isr77, kcode_segment, flags, 0);
    idt_set_gate(78, (uint64_t)isr78, kcode_segment, flags, 0);
    idt_set_gate(79, (uint64_t)isr79, kcode_segment, flags, 0);
    idt_set_gate(80, (uint64_t)isr80, kcode_segment, flags, 0);
    idt_set_gate(81, (uint64_t)isr81, kcode_segment, flags, 0);
    idt_set_gate(82, (uint64_t)isr82, kcode_segment, flags, 0);
    idt_set_gate(83, (uint64_t)isr83, kcode_segment, flags, 0);
    idt_set_gate(84, (uint64_t)isr84, kcode_segment, flags, 0);
    idt_set_gate(85, (uint64_t)isr85, kcode_segment, flags, 0);
    idt_set_gate(86, (uint64_t)isr86, kcode_segment, flags, 0);
    idt_set_gate(87, (uint64_t)isr87, kcode_segment, flags, 0);
    idt_set_gate(88, (uint64_t)isr88, kcode_segment, flags, 0);
    idt_set_gate(89, (uint64_t)isr89, kcode_segment, flags, 0);
    idt_set_gate(90, (uint64_t)isr90, kcode_segment, flags, 0);
    idt_set_gate(91, (uint64_t)isr91, kcode_segment, flags, 0);
    idt_set_gate(92, (uint64_t)isr92, kcode_segment, flags, 0);
    idt_set_gate(93, (uint64_t)isr93, kcode_segment, flags, 0);
    idt_set_gate(94, (uint64_t)isr94, kcode_segment, flags, 0);
    idt_set_gate(95, (uint64_t)isr95, kcode_segment, flags, 0);
    idt_set_gate(96, (uint64_t)isr96, kcode_segment, flags, 0);
    idt_set_gate(97, (uint64_t)isr97, kcode_segment, flags, 0);
    idt_set_gate(98, (uint64_t)isr98, kcode_segment, flags, 0);
    idt_set_gate(99, (uint64_t)isr99, kcode_segment, flags, 0);
    idt_set_gate(100, (uint64_t)isr100, kcode_segment, flags, 0);
    idt_set_gate(101, (uint64_t)isr101, kcode_segment, flags, 0);
    idt_set_gate(102, (uint64_t)isr102, kcode_segment, flags, 0);
    idt_set_gate(103, (uint64_t)isr103, kcode_segment, flags, 0);
    idt_set_gate(104, (uint64_t)isr104, kcode_segment, flags, 0);
    idt_set_gate(105, (uint64_t)isr105, kcode_segment, flags, 0);
    idt_set_gate(106, (uint64_t)isr106, kcode_segment, flags, 0);
    idt_set_gate(107, (uint64_t)isr107, kcode_segment, flags, 0);
    idt_set_gate(108, (uint64_t)isr108, kcode_segment, flags, 0);
    idt_set_gate(109, (uint64_t)isr109, kcode_segment, flags, 0);
    idt_set_gate(110, (uint64_t)isr110, kcode_segment, flags, 0);
    idt_set_gate(111, (uint64_t)isr111, kcode_segment, flags, 0);
    idt_set_gate(112, (uint64_t)isr112, kcode_segment, flags, 0);
    idt_set_gate(113, (uint64_t)isr113, kcode_segment, flags, 0);
    idt_set_gate(114, (uint64_t)isr114, kcode_segment, flags, 0);
    idt_set_gate(115, (uint64_t)isr115, kcode_segment, flags, 0);
    idt_set_gate(116, (uint64_t)isr116, kcode_segment, flags, 0);
    idt_set_gate(117, (uint64_t)isr117, kcode_segment, flags, 0);
    idt_set_gate(118, (uint64_t)isr118, kcode_segment, flags, 0);
    idt_set_gate(119, (uint64_t)isr119, kcode_segment, flags, 0);
    idt_set_gate(120, (uint64_t)isr120, kcode_segment, flags, 0);
    idt_set_gate(121, (uint64_t)isr121, kcode_segment, flags, 0);
    idt_set_gate(122, (uint64_t)isr122, kcode_segment, flags, 0);
    idt_set_gate(123, (uint64_t)isr123, kcode_segment, flags, 0);
    idt_set_gate(124, (uint64_t)isr124, kcode_segment, flags, 0);
    idt_set_gate(125, (uint64_t)isr125, kcode_segment, flags, 0);
    idt_set_gate(126, (uint64_t)isr126, kcode_segment, flags, 0);
    idt_set_gate(127, (uint64_t)isr127, kcode_segment, flags, 0);
    idt_set_gate(128, (uint64_t)isr128, kcode_segment, flags, 0);
    idt_set_gate(129, (uint64_t)isr129, kcode_segment, flags, 0);
    idt_set_gate(130, (uint64_t)isr130, kcode_segment, flags, 0);
    idt_set_gate(131, (uint64_t)isr131, kcode_segment, flags, 0);
    idt_set_gate(132, (uint64_t)isr132, kcode_segment, flags, 0);
    idt_set_gate(133, (uint64_t)isr133, kcode_segment, flags, 0);
    idt_set_gate(134, (uint64_t)isr134, kcode_segment, flags, 0);
    idt_set_gate(135, (uint64_t)isr135, kcode_segment, flags, 0);
    idt_set_gate(136, (uint64_t)isr136, kcode_segment, flags, 0);
    idt_set_gate(137, (uint64_t)isr137, kcode_segment, flags, 0);
    idt_set_gate(138, (uint64_t)isr138, kcode_segment, flags, 0);
    idt_set_gate(139, (uint64_t)isr139, kcode_segment, flags, 0);
    idt_set_gate(140, (uint64_t)isr140, kcode_segment, flags, 0);
    idt_set_gate(141, (uint64_t)isr141, kcode_segment, flags, 0);
    idt_set_gate(142, (uint64_t)isr142, kcode_segment, flags, 0);
    idt_set_gate(143, (uint64_t)isr143, kcode_segment, flags, 0);
    idt_set_gate(144, (uint64_t)isr144, kcode_segment, flags, 0);
    idt_set_gate(145, (uint64_t)isr145, kcode_segment, flags, 0);
    idt_set_gate(146, (uint64_t)isr146, kcode_segment, flags, 0);
    idt_set_gate(147, (uint64_t)isr147, kcode_segment, flags, 0);
    idt_set_gate(148, (uint64_t)isr148, kcode_segment, flags, 0);
    idt_set_gate(149, (uint64_t)isr149, kcode_segment, flags, 0);
    idt_set_gate(150, (uint64_t)isr150, kcode_segment, flags, 0);
    idt_set_gate(151, (uint64_t)isr151, kcode_segment, flags, 0);
    idt_set_gate(152, (uint64_t)isr152, kcode_segment, flags, 0);
    idt_set_gate(153, (uint64_t)isr153, kcode_segment, flags, 0);
    idt_set_gate(154, (uint64_t)isr154, kcode_segment, flags, 0);
    idt_set_gate(155, (uint64_t)isr155, kcode_segment, flags, 0);
    idt_set_gate(156, (uint64_t)isr156, kcode_segment, flags, 0);
    idt_set_gate(157, (uint64_t)isr157, kcode_segment, flags, 0);
    idt_set_gate(158, (uint64_t)isr158, kcode_segment, flags, 0);
    idt_set_gate(159, (uint64_t)isr159, kcode_segment, flags, 0);
    idt_set_gate(160, (uint64_t)isr160, kcode_segment, flags, 0);
    idt_set_gate(161, (uint64_t)isr161, kcode_segment, flags, 0);
    idt_set_gate(162, (uint64_t)isr162, kcode_segment, flags, 0);
    idt_set_gate(163, (uint64_t)isr163, kcode_segment, flags, 0);
    idt_set_gate(164, (uint64_t)isr164, kcode_segment, flags, 0);
    idt_set_gate(165, (uint64_t)isr165, kcode_segment, flags, 0);
    idt_set_gate(166, (uint64_t)isr166, kcode_segment, flags, 0);
    idt_set_gate(167, (uint64_t)isr167, kcode_segment, flags, 0);
    idt_set_gate(168, (uint64_t)isr168, kcode_segment, flags, 0);
    idt_set_gate(169, (uint64_t)isr169, kcode_segment, flags, 0);
    idt_set_gate(170, (uint64_t)isr170, kcode_segment, flags, 0);
    idt_set_gate(171, (uint64_t)isr171, kcode_segment, flags, 0);
    idt_set_gate(172, (uint64_t)isr172, kcode_segment, flags, 0);
    idt_set_gate(173, (uint64_t)isr173, kcode_segment, flags, 0);
    idt_set_gate(174, (uint64_t)isr174, kcode_segment, flags, 0);
    idt_set_gate(175, (uint64_t)isr175, kcode_segment, flags, 0);
    idt_set_gate(176, (uint64_t)isr176, kcode_segment, flags, 0);
    idt_set_gate(177, (uint64_t)isr177, kcode_segment, flags, 0);
    idt_set_gate(178, (uint64_t)isr178, kcode_segment, flags, 0);
    idt_set_gate(179, (uint64_t)isr179, kcode_segment, flags, 0);
    idt_set_gate(180, (uint64_t)isr180, kcode_segment, flags, 0);
    idt_set_gate(181, (uint64_t)isr181, kcode_segment, flags, 0);
    idt_set_gate(182, (uint64_t)isr182, kcode_segment, flags, 0);
    idt_set_gate(183, (uint64_t)isr183, kcode_segment, flags, 0);
    idt_set_gate(184, (uint64_t)isr184, kcode_segment, flags, 0);
    idt_set_gate(185, (uint64_t)isr185, kcode_segment, flags, 0);
    idt_set_gate(186, (uint64_t)isr186, kcode_segment, flags, 0);
    idt_set_gate(187, (uint64_t)isr187, kcode_segment, flags, 0);
    idt_set_gate(188, (uint64_t)isr188, kcode_segment, flags, 0);
    idt_set_gate(189, (uint64_t)isr189, kcode_segment, flags, 0);
    idt_set_gate(190, (uint64_t)isr190, kcode_segment, flags, 0);
    idt_set_gate(191, (uint64_t)isr191, kcode_segment, flags, 0);
    idt_set_gate(192, (uint64_t)isr192, kcode_segment, flags, 0);
    idt_set_gate(193, (uint64_t)isr193, kcode_segment, flags, 0);
    idt_set_gate(194, (uint64_t)isr194, kcode_segment, flags, 0);
    idt_set_gate(195, (uint64_t)isr195, kcode_segment, flags, 0);
    idt_set_gate(196, (uint64_t)isr196, kcode_segment, flags, 0);
    idt_set_gate(197, (uint64_t)isr197, kcode_segment, flags, 0);
    idt_set_gate(198, (uint64_t)isr198, kcode_segment, flags, 0);
    idt_set_gate(199, (uint64_t)isr199, kcode_segment, flags, 0);
    idt_set_gate(200, (uint64_t)isr200, kcode_segment, flags, 0);
    idt_set_gate(201, (uint64_t)isr201, kcode_segment, flags, 0);
    idt_set_gate(202, (uint64_t)isr202, kcode_segment, flags, 0);
    idt_set_gate(203, (uint64_t)isr203, kcode_segment, flags, 0);
    idt_set_gate(204, (uint64_t)isr204, kcode_segment, flags, 0);
    idt_set_gate(205, (uint64_t)isr205, kcode_segment, flags, 0);
    idt_set_gate(206, (uint64_t)isr206, kcode_segment, flags, 0);
    idt_set_gate(207, (uint64_t)isr207, kcode_segment, flags, 0);
    idt_set_gate(208, (uint64_t)isr208, kcode_segment, flags, 0);
    idt_set_gate(209, (uint64_t)isr209, kcode_segment, flags, 0);
    idt_set_gate(210, (uint64_t)isr210, kcode_segment, flags, 0);
    idt_set_gate(211, (uint64_t)isr211, kcode_segment, flags, 0);
    idt_set_gate(212, (uint64_t)isr212, kcode_segment, flags, 0);
    idt_set_gate(213, (uint64_t)isr213, kcode_segment, flags, 0);
    idt_set_gate(214, (uint64_t)isr214, kcode_segment, flags, 0);
    idt_set_gate(215, (uint64_t)isr215, kcode_segment, flags, 0);
    idt_set_gate(216, (uint64_t)isr216, kcode_segment, flags, 0);
    idt_set_gate(217, (uint64_t)isr217, kcode_segment, flags, 0);
    idt_set_gate(218, (uint64_t)isr218, kcode_segment, flags, 0);
    idt_set_gate(219, (uint64_t)isr219, kcode_segment, flags, 0);
    idt_set_gate(220, (uint64_t)isr220, kcode_segment, flags, 0);
    idt_set_gate(221, (uint64_t)isr221, kcode_segment, flags, 0);
    idt_set_gate(222, (uint64_t)isr222, kcode_segment, flags, 0);
    idt_set_gate(223, (uint64_t)isr223, kcode_segment, flags, 0);
    idt_set_gate(224, (uint64_t)isr224, kcode_segment, flags, 0);
    idt_set_gate(225, (uint64_t)isr225, kcode_segment, flags, 0);
    idt_set_gate(226, (uint64_t)isr226, kcode_segment, flags, 0);
    idt_set_gate(227, (uint64_t)isr227, kcode_segment, flags, 0);
    idt_set_gate(228, (uint64_t)isr228, kcode_segment, flags, 0);
    idt_set_gate(229, (uint64_t)isr229, kcode_segment, flags, 0);
    idt_set_gate(230, (uint64_t)isr230, kcode_segment, flags, 0);
    idt_set_gate(231, (uint64_t)isr231, kcode_segment, flags, 0);
    idt_set_gate(232, (uint64_t)isr232, kcode_segment, flags, 0);
    idt_set_gate(233, (uint64_t)isr233, kcode_segment, flags, 0);
    idt_set_gate(234, (uint64_t)isr234, kcode_segment, flags, 0);
    idt_set_gate(235, (uint64_t)isr235, kcode_segment, flags, 0);
    idt_set_gate(236, (uint64_t)isr236, kcode_segment, flags, 0);
    idt_set_gate(237, (uint64_t)isr237, kcode_segment, flags, 0);
    idt_set_gate(238, (uint64_t)isr238, kcode_segment, flags, 0);
    idt_set_gate(239, (uint64_t)isr239, kcode_segment, flags, 0);
    idt_set_gate(240, (uint64_t)isr240, kcode_segment, flags, 0);
    idt_set_gate(241, (uint64_t)isr241, kcode_segment, flags, 0);
    idt_set_gate(242, (uint64_t)isr242, kcode_segment, flags, 0);
    idt_set_gate(243, (uint64_t)isr243, kcode_segment, flags, 0);
    idt_set_gate(244, (uint64_t)isr244, kcode_segment, flags, 0);
    idt_set_gate(245, (uint64_t)isr245, kcode_segment, flags, 0);
    idt_set_gate(246, (uint64_t)isr246, kcode_segment, flags, 0);
    idt_set_gate(247, (uint64_t)isr247, kcode_segment, flags, 0);
    idt_set_gate(248, (uint64_t)isr248, kcode_segment, flags, 0);
    idt_set_gate(249, (uint64_t)isr249, kcode_segment, flags, 0);
    idt_set_gate(250, (uint64_t)isr250, kcode_segment, flags, 0);
    idt_set_gate(251, (uint64_t)isr251, kcode_segment, flags, 0);
    idt_set_gate(252, (uint64_t)isr252, kcode_segment, flags, 0);
    idt_set_gate(253, (uint64_t)isr253, kcode_segment, flags, 0);
    idt_set_gate(254, (uint64_t)isr254, kcode_segment, flags, 0);
    idt_set_gate(255, (uint64_t)isr255, kcode_segment, flags, 0);

    idt_load(&g_IdtPtr);
    
    printk(IDT_CLASS "256 ISR installed and loaded\n");

    return 0;
}