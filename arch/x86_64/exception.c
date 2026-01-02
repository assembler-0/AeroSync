/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/exception.c
 * @brief Exception helper functions
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <arch/x86_64/exception.h>
#include <lib/string.h>

void get_exception_as_str(char* buff, uint32_t num) {
    if (!buff || num > 31) return;
    switch (num) {
        case 0: 
            strcpy(buff, "Divide by Zero");
            break;
        case 1: 
            strcpy(buff, "Debug");
            break;
        case 2: 
            strcpy(buff, "NMI");
            break;
        case 3: 
            strcpy(buff, "Breakpoint");
            break;
        case 4: 
            strcpy(buff, "Overflow");
            break;
        case 5: 
            strcpy(buff, "Bound Range Exceeded");
            break;
        case 6: 
            strcpy(buff, "Invalid Opcode");
            break;
        case 7: 
            strcpy(buff, "Device Not Available");
            break;
        case 8: 
            strcpy(buff, "Double Fault");
            break;
        case 9: 
            strcpy(buff, "Coprocessor Segment Overrun");
            break;
        case 10: 
            strcpy(buff, "Invalid TSS");
            break;
        case 11: 
            strcpy(buff, "Segment Not Present");
            break;
        case 12: 
            strcpy(buff, "Stack Fault");
            break;
        case 13: 
            strcpy(buff, "General Protection Fault");
            break;
        case 14: 
            strcpy(buff, "Page Fault");
            break;
        case 15: 
            strcpy(buff, "Reserved");
            break;
        case 16: 
            strcpy(buff, "x87 FPU Floating-Point exception");
            break;
        case 17: 
            strcpy(buff, "Alignment Check");
            break;
        case 18: 
            strcpy(buff, "Machine Check");
            break;
        case 19: 
            strcpy(buff, "SIMD Floating-Point exception");
            break;
        case 20: 
            strcpy(buff, "Virtualization exception");
            break;
        case 21: 
            strcpy(buff, "Control protocol exception");
            break;
        case 22 ... 27: 
            strcpy(buff, "Reserved");
            break;
        case 28: 
            strcpy(buff, "Hypervisor injection exception");
            break;
        case 29: 
            strcpy(buff, "VMM communication exception");
            break;
        case 30: 
            strcpy(buff, "Security exception");
            break;
        case 31: 
            strcpy(buff, "Reserved");
            break;
        default:
            strcpy(buff, "Unknown Exception");
            break;
        }
    }
    
    uint64_t search_exception_table(uint64_t addr) {
        struct exception_table_entry *entry;
        
        for (entry = __start___ex_table; entry < __stop___ex_table; entry++) {
            if (entry->insn == addr) {
                return entry->fixup;
            }
        }
        
        return 0;
    }
    