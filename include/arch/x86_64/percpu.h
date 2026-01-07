/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>

/* Linker symbols for the per-CPU section */
extern char _percpu_start[];
extern char _percpu_end[];

/* Per-CPU offset array */
extern unsigned long __per_cpu_offset[MAX_CPUS];

/* Macro to define a per-CPU variable */
#define DEFINE_PER_CPU(type, name)                                             \
  __attribute__((section(".percpu"), used)) type name

/* Macro to declare a per-CPU variable */
#define DECLARE_PER_CPU(type, name)                                            \
  extern __attribute__((section(".percpu"))) type name

/*
 * Per-CPU accessors using GS segment override.
 * We use typeof(var) to determing the size.
 */

/*
 * Force a read from the per-CPU area for variable 'var'.
 * Example: val = this_cpu_read(my_percpu_var);
 */
#define this_cpu_read(var)                                                     \
  ({                                                                           \
    typeof(var) __ret;                                                         \
    switch (sizeof(var)) {                                                     \
    case 1:                                                                    \
      asm volatile("movb %%gs:%1, %0" : "=q"(__ret) : "m"(var));               \
      break;                                                                   \
    case 2:                                                                    \
      asm volatile("movw %%gs:%1, %0" : "=r"(__ret) : "m"(var));               \
      break;                                                                   \
    case 4:                                                                    \
      asm volatile("movl %%gs:%1, %0" : "=r"(__ret) : "m"(var));               \
      break;                                                                   \
    case 8:                                                                    \
      asm volatile("movq %%gs:%1, %0" : "=r"(__ret) : "m"(var));               \
      break;                                                                   \
    default:                                                                   \
      __builtin_unreachable();                                                 \
    }                                                                          \
    __ret;                                                                     \
  })

/*
 * Write a value to a per-CPU variable.
 * Example: this_cpu_write(my_percpu_var, 123);
 */
#define this_cpu_write(var, val)                                               \
  do {                                                                         \
    typeof(var) __val = (typeof(var))(val);                                    \
    switch (sizeof(var)) {                                                     \
    case 1:                                                                    \
      asm volatile("movb %1, %%gs:%0" : "+m"(var) : "qi"(__val));              \
      break;                                                                   \
    case 2:                                                                    \
      asm volatile("movw %1, %%gs:%0" : "+m"(var) : "ri"(__val));              \
      break;                                                                   \
    case 4:                                                                    \
      asm volatile("movl %1, %%gs:%0" : "+m"(var) : "ri"(__val));              \
      break;                                                                   \
    case 8:                                                                    \
      asm volatile("movq %1, %%gs:%0" : "+m"(var) : "re"(__val));              \
      break;                                                                   \
    default:                                                                   \
      __builtin_unreachable();                                                 \
    }                                                                          \
  } while (0)

#define this_cpu_add(var, val)                                                 \
  do {                                                                         \
    typeof(var) __val = (typeof(var))(val);                                    \
    switch (sizeof(var)) {                                                     \
    case 1:                                                                    \
      asm volatile("addb %1, %%gs:%0" : "+m"(var) : "qi"(__val));              \
      break;                                                                   \
    case 2:                                                                    \
      asm volatile("addw %1, %%gs:%0" : "+m"(var) : "ri"(__val));              \
      break;                                                                   \
    case 4:                                                                    \
      asm volatile("addl %1, %%gs:%0" : "+m"(var) : "ri"(__val));              \
      break;                                                                   \
    case 8:                                                                    \
      asm volatile("addq %1, %%gs:%0" : "+m"(var) : "re"(__val));              \
      break;                                                                   \
    default:                                                                   \
      __builtin_unreachable();                                                 \
    }                                                                          \
  } while (0)

#define this_cpu_inc(var) this_cpu_add(var, 1)
#define this_cpu_dec(var) this_cpu_add(var, -1)

/*
 * Double-width cmpxchg on per-CPU variables.
 * Targets two adjacent 64-bit values (16 bytes total).
 * Must be 16-byte aligned.
 */
#define this_cpu_cmpxchg_double(pcp1, pcp2, o1, o2, n1, n2)                    \
  ({                                                                           \
    bool __ret;                                                                \
    asm volatile("lock; cmpxchg16b %%gs:%3; setz %0"                           \
                 : "=a"(__ret), "+d"(o2), "+a"(o1)                             \
                 : "m"(pcp1), "b"(n1), "c"(n2), "1"(o2), "2"(o1)               \
                 : "memory");                                                  \
    __ret;                                                                     \
  })

/*
 * Get the address of a per-CPU variable for the CURRENT CPU.
 * Note: This generally returns a pointer relative to GS base (0-based usually).
 * BUT, since we set GS base to the linear address of the per-cpu area,
 * taking the address of 'var' (which is 0-based in the linker) directly
 * might not give the linear address if compiler decides to use RIP-relative
 * addressing to the symbol.
 *
 * Actually, 'var' symbol address is 0 + offset.
 * We want: GS_BASE + offset.
 *
 * However, simpler approach:
 * The symbol 'var' has address X (small offset).
 * The per-cpu area is at P.
 * GS_BASE is P - 0 (so GS points to P).
 *
 * If we want the valid linear pointer to the variable for the current CPU/GS:
 * We can read the 'self' pointer from per-cpu area, or just add offset to
 * per-cpu base.
 *
 * A common trick:
 *   unsigned long offset;
 *   asm("mov %%gs:0, %0" : "=r"(offset)); // If we store 'this_cpu_off' at 0
 *   return (type *)(offset + (unsigned long)&var);
 *
 * FOR NOW: simple implementation that relies on 'this_cpu_off' being at the
 * start of per-cpu area or similar mechanism if we want pointer arithmetic.
 *
 * BUT, a simpler "this_cpu_ptr" needs the per-cpu area base.
 * Let's implement 'this_cpu_read/write' first. 'this_cpu_ptr' is trickier
 * without a defined 'self' variable at the start.
 *
 * Let's define a 'this_cpu_off' variable later in percpu.c that holds the
 * offset/self pointer if needed.
 */
#define this_cpu_ptr(var)                                                      \
  ((typeof(&(var)))((unsigned long)&(var) + __per_cpu_offset[smp_get_id()]))

/*
 * Get the address of a per-CPU variable for a SPECIFIC CPU.
 */
#define per_cpu_ptr(var, cpu)                                                  \
  ((typeof(&(var)))((unsigned long)&(var) + __per_cpu_offset[(cpu)]))

// Function prototypes for setup
void setup_per_cpu_areas(void);

bool percpu_ready(void);