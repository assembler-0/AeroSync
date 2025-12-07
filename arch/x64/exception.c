#include <lib/string.h>

void get_exception_as_str(char* buff, uint32_t num) {
    if (!buff || num > 31 || num < 0) return;
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