#pragma once

/* ========================
 * BASIC COMPILER DETECTION
 * ======================== */
#if defined(__clang__)
#  define COMPILER_CLANG 1
#elif defined(__GNUC__)
#  define COMPILER_GCC 1
#else
#  error "Unsupported compiler"
#endif

/* ========================
 * FUNCTION ATTRIBUTES
 * ======================== */

#define __noreturn      __attribute__((noreturn))
#define __noinline      __attribute__((noinline))
#define __always_inline __attribute__((always_inline))
#define __flatten       __attribute__((flatten))
#define __hot           __attribute__((hot))
#define __cold          __attribute__((cold))
#define __unused        __attribute__((unused))
#define __used          __attribute__((used))
#define __nonnull(x)    __attribute__((nonnull(x)))
#define __finline        __attribute__((always_inline))
#define __optimize(x)   __attribute__((optimize(x)))

/* ========================
 * MEMORY / LAYOUT ATTRIBUTES
 * ======================== */

#define __aligned(x)    __attribute__((aligned(x)))
#define __packed        __attribute__((packed))
#define __weak          __attribute__((weak))
#define __alias(x)      __attribute__((alias(x)))
#define __section(x)    __attribute__((section(x)))
#define __visibility(x) __attribute__((visibility(x)))
#define __attrib(x)     __attribute__((x))

/* ========================
 * ABI ATTRIBUTES (UEFI!)
 * ======================== */

#if defined(__x86_64__)
#  define __ms_abi      __attribute__((ms_abi))
#  define __sysv_abi    __attribute__((sysv_abi))
#else
#  define __ms_abi
#  define __sysv_abi
#endif

/* ========================
 * INIT / EXIT SECTIONS
 *   Similar to Linux’s .initcall
 * ======================== */

#define __init          __section(".init.text") __cold
#define __init_data     __section(".init.data")
#define __late_init     __section(".late_init")
#define __exit          __section(".exit.text") __cold

/* ========================
 * BRANCHING / FLOW
 * ======================== */

#define __fallthrough   __attribute__((fallthrough))
#define __unreachable() __builtin_unreachable()
#define __likely(x)     __builtin_expect(!!(x), 1)
#define __unlikely(x)   __builtin_expect(!!(x), 0)

/*
 * Branch Prediction Hints
 * Tell the compiler which path is the "Hot Path" so it can optimize assembly layout.
 * likely(x):   We expect x to be TRUE (1)
 * unlikely(x): We expect x to be FALSE (0)
 */
#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

/*
 * Memory Barriers
 * barrier(): Prevents the compiler from reordering instructions across this point.
 *            It does NOT prevent the CPU from reordering.
 */
#define barrier()      asm volatile("" ::: "memory")

/*
 * READ_ONCE / WRITE_ONCE
 *
 * These prevent the compiler from:
 * 1. Merging accesses (store tearing/fusing)
 * 2. Reloading the value from memory multiple times (cache in register)
 * 3. Reordering the access relative to other code
 *
 * This is done by casting to 'volatile'.
 */

// Force a read from memory (bypass register cache)
#define READ_ONCE(x) (*(const volatile typeof(x) *)&(x))

// Force a write to memory (bypass register cache/deferral)
#define WRITE_ONCE(x, val) \
do { \
*(volatile typeof(x) *)&(x) = (val); \
} while (0)

/*
 * If you are porting rbtree, you might run into these too.
 * For x86_64 (TSO - Total Store Order), simple barriers are usually enough
 * for a hobby kernel, but technically you need proper fencing instructions
 * for SMP.
 */
#define smp_mb()    asm volatile("lock; addl $0, -4(%%rsp)" ::: "memory", "cc")
#define smp_rmb()   barrier() // x86 loads are ordered
#define smp_wmb()   barrier() // x86 stores are ordered

/* ========================
 * ALIGNMENT HELPERS
 * ======================== */

#define ALIGN(x, a)             (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)          ALIGN(x, a)

#define __percpu
#define __rcu