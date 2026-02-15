#pragma once
#include <aerosync/types.h>

/* ========================
 * BASIC PLATFORM DETECTION
 * ======================== */
#if defined(__clang__)
#  define COMPILER_CLANG 1
#elif defined(__GNUC__)
#  warning "GCC is not officially supported (yet)"
#else
#  warning "Unsupported compiler: " __VERSION__
#endif

#if defined(__x86_64__)
#  define ARCH_X86_64 1
#else
#  warning "Unsupported architecture: " __TARGET__
#endif

/* ========================
 * FUNCTION ATTRIBUTES
 * ======================== */

#define __noreturn      __attribute__((__noreturn__))
#define __noinline      __attribute__((__noinline__))
#define noinline        __noinline
#define __always_inline __attribute__((__always_inline__))
#define __flatten       __attribute__((__flatten__))
#define __hot           __attribute__((__hot__))
#define __cold          __attribute__((__cold__))
#define __unused        __attribute__((__unused__))
#define __used          __attribute__((__used__))
#define __nonnull(x)    __attribute__((__nonnull__(x)))
#define __finline        __attribute__((__always_inline__))
#define __optimize(x)   __attribute__((__optimize__(x)))
#define __deprecated    __attribute__((__deprecated__))
/* include/linux/compiler_attributes.h:368 */
#define __must_check    __attribute__((__warn_unused_result__))
#define __no_sanitize   __attribute__((no_sanitize("undefined", "address", "integer", "null", "bounds", "vla-bound", "object-size")))
#define __no_cfi        __attribute__((no_sanitize("cfi")))

/* ========================
 * MEMORY / LAYOUT ATTRIBUTES
 * ======================== */

#define __aligned(x)    __attribute__((__aligned__(x)))
#define __packed        __attribute__((__packed__))
#define __weak          __attribute__((__weak__))
#define __alias(x)      __attribute__((__alias__(x)))
#define __section(x)    __attribute__((__section__(x)))
#define __visibility(x) __attribute__((__visibility__(x)))
#define __attrib(x)     __attribute__((x))

/* ========================
 * ABI ATTRIBUTES (UEFI!)
 * ======================== */

#if defined(__x86_64__)
#  define __ms_abi      __attribute__((ms_abi))
#  define __sysv_abi    __attribute__((sysv_abi))
#  define __cdecl       __attribute__((cdecl))
#  define __stdcall     __attribute__((stdcall))
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
#define fallthrough
#define __unreachable() __builtin_unreachable()
#define __trap()        __builtin_trap()
#define __likely(x)     __builtin_expect(!!(x), 1)
#define __unlikely(x)   __builtin_expect(!!(x), 0)

/*
 * Branch Prediction Hints
 * Tell the compiler which path is the "Hot Path" so it can optimize assembly layout.
 * likely(x):   We expect x to be TRUE (1)
 * unlikely(x): We expect x to be FALSE (0)
 */
#define likely(x)      __likely(x)
#define unlikely(x)    __unlikely(x)

/*
 * Memory Barriers
 * barrier(): Prevents the compiler from reordering instructions across this point.
 *            It does NOT prevent the CPU from reordering.
 */
#define cbarrier()      __asm__ volatile("" ::: "memory")

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

/* ========================
 * ALIGNMENT HELPERS
 * ======================== */

#define ALIGN(x, a)             (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)          ALIGN(x, a)

#define __STRINGIFY(x) #x
#define STRINGIFY(x) __STRINGIFY(x)

/* ETC. */
#define static_assert _Static_assert
#define __percpu
#define __rcu
#define __force

#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e); }))

#ifdef COMPILER_CLANG

/*
 * useful for clang sanitizers
 */

struct SourceLocation {
  const char *file;
  uint32_t line;
  uint32_t column;
};

struct TypeDescriptor {
  uint16_t type_kind;
  uint16_t type_info;
  char type_name[];
};

#endif