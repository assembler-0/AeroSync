#pragma once

#include <kernel/types.h>

#define _full_mem_prot_start() {\
    __sync_synchronize();\
    __asm__ volatile("mfence; sfence; lfence" ::: "memory");\
}
#define _full_mem_prot_end() {\
    __asm__ volatile("mfence; sfence; lfence" ::: "memory");\
    __sync_synchronize();\
}

#define barrier()       __asm__ __volatile__("" ::: "memory")
#define cpu_relax()     __asm__ __volatile__("pause" ::: "memory")
#define cpu_isync()     __asm__ __volatile__("sfence" ::: "memory")
#define cpu_hlt()       __asm__ __volatile__("hlt" ::: "memory")
#define cpu_cli()       __asm__ __volatile__("cli" ::: "memory")
#define cpu_sti()       __asm__ __volatile__("sti" ::: "memory")
#define system_hlt()    do { cpu_cli(); cpu_hlt(); } while (1)

uint64_t rdtsc();
uint64_t rdtscp();

typedef uint64_t irq_flags_t;

irq_flags_t save_irq_flags(void);
void restore_irq_flags(irq_flags_t flags);

// CPUID detection
void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);

// MSR access
uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t value);
