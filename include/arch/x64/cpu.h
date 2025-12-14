#pragma once

#include <compiler.h>
#include <kernel/types.h>

#define _full_mem_prot_start()                                                 \
  {                                                                            \
    __sync_synchronize();                                                      \
    __asm__ volatile("mfence; sfence; lfence" ::: "memory");                   \
  }
#define _full_mem_prot_end()                                                   \
  {                                                                            \
    __asm__ volatile("mfence; sfence; lfence" ::: "memory");                   \
    __sync_synchronize();                                                      \
  }

#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")
#define cpu_isync() __asm__ __volatile__("sfence" ::: "memory")
#define cpu_hlt() __asm__ __volatile__("hlt" ::: "memory")
#define cpu_cli() __asm__ __volatile__("cli" ::: "memory")
#define cpu_sti() __asm__ __volatile__("sti" ::: "memory")
#define system_hlt()                                                           \
  do {                                                                         \
    cpu_cli();                                                                 \
    cpu_hlt();                                                                 \
  } while (1)

typedef uint64_t irq_flags_t;

irq_flags_t save_irq_flags(void);
void restore_irq_flags(irq_flags_t flags);

///@warning DO NOT TOUCH THIS STRUCTURE!!
typedef struct cpu_regs {
  // Pushed by ISR (Segs) - Last pushed, so at lowest address (Offset 0)
  uint64_t ds, es, fs, gs;

  // Pushed by ISR (GPRs)
  uint64_t rax, rbx, rcx, rdx, rdi, rsi, rbp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

  // Pushed by ISR (Int Info)
  uint64_t interrupt_number;
  uint64_t error_code;

  // Pushed by CPU (Exception Frame)
  uint64_t rip, cs, rflags;
  uint64_t rsp, ss;
} __packed cpu_regs;

// CPUID detection
void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
           uint32_t *edx);
void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);

// MSR access
uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t value);
