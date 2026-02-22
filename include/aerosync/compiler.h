#pragma once
#include <aerosync/types.h>

#if defined(__clang__)
/* fixme */
#elif defined(__GNUC__)
/* fixme */
#else
#  warning "Unsupported compiler: " __VERSION__
#endif

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
#define __attribute_const__ __attribute__((__const__))
/* include/linux/compiler_attributes.h:368 */
#define __must_check    __attribute__((__warn_unused_result__))
#define __no_sanitize   __attribute__((no_sanitize("undefined", "address", "integer", "null", "bounds", "vla-bound", "object-size")))
#define __no_cfi        __attribute__((no_sanitize("cfi")))

#define __aligned(x)    __attribute__((__aligned__(x)))
#define __packed        __attribute__((__packed__))
#define __weak          __attribute__((__weak__))
#define __alias(x)      __attribute__((__alias__(x)))
#define __section(x)    __attribute__((__section__(x)))
#define __visibility(x) __attribute__((__visibility__(x)))
#define __attrib(x)     __attribute__((x))
#define is_signed_type(type) (((type)(-1)) < (__force type)1)
#define is_unsigned_type(type) (!is_signed_type(type))

#if defined(__x86_64__)
#  define __ms_abi      __attribute__((ms_abi))
#  define __sysv_abi    __attribute__((sysv_abi))
#  define __cdecl       __attribute__((cdecl))
#  define __stdcall     __attribute__((stdcall))
#else
#  define __ms_abi
#  define __sysv_abi
#endif

#define __init          __section(".init.text") __cold
#define __init_data     __section(".init.data")
#define __late_init     __section(".late_init")
#define __exit          __section(".exit.text") __cold

#define __fallthrough   __attribute__((fallthrough))
#define fallthrough
#define __unreachable() __builtin_unreachable()
#define __trap()        __builtin_trap()
#define __likely(x)     __builtin_expect(!!(x), 1)
#define __unlikely(x)   __builtin_expect(!!(x), 0)
#define __nonstring     __attribute__((__nonstring__))

/*
 * Branch Prediction Hints
 * Tell the compiler which path is the "Hot Path" so it can optimize assembly layout.
 * likely(x):   We expect x to be TRUE (1)
 * unlikely(x): We expect x to be FALSE (0)
 */

#define likely(x)      __likely(x)
#define unlikely(x)    __unlikely(x)

# define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

/* &a[0] degrades to a pointer: a different type from an array */
#define __is_array(a)		(!__same_type((a), &(a)[0]))
#define __must_be_array(a)	__BUILD_BUG_ON_ZERO_MSG(!__is_array(a), \
							"must be array")

#define __is_byte_array(a)	(__is_array(a) && sizeof((a)[0]) == 1)
#define __must_be_byte_array(a)	__BUILD_BUG_ON_ZERO_MSG(!__is_byte_array(a), \
							"must be byte array")

/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 *
 * Details:
 * - sizeof() return an integer constant expression, and does not evaluate
 *   the value of its operand; it only examines the type of its operand.
 * - The results of comparing two integer constant expressions is also
 *   an integer constant expression.
 * - The first literal "8" isn't important. It could be any literal value.
 * - The second literal "8" is to avoid warnings about unaligned pointers;
 *   this could otherwise just be "1".
 * - (long)(x) is used to avoid warnings about 64-bit types on 32-bit
 *   architectures.
 * - The C Standard defines "null pointer constant", "(void *)0", as
 *   distinct from other void pointers.
 * - If (x) is an integer constant expression, then the "* 0l" resolves
 *   it into an integer constant expression of value 0. Since it is cast to
 *   "void *", this makes the second operand a null pointer constant.
 * - If (x) is not an integer constant expression, then the second operand
 *   resolves to a void pointer (but not a null pointer constant: the value
 *   is not an integer constant 0).
 * - The conditional operator's third operand, "(int *)8", is an object
 *   pointer (to type "int").
 * - The behavior (including the return type) of the conditional operator
 *   ("operand1 ? operand2 : operand3") depends on the kind of expressions
 *   given for the second and third operands. This is the central mechanism
 *   of the macro:
 *   - When one operand is a null pointer constant (i.e. when x is an integer
 *     constant expression) and the other is an object pointer (i.e. our
 *     third operand), the conditional operator returns the type of the
 *     object pointer operand (i.e. "int *"). Here, within the sizeof(), we
 *     would then get:
 *       sizeof(*((int *)(...))  == sizeof(int)  == 4
 *   - When one operand is a void pointer (i.e. when x is not an integer
 *     constant expression) and the other is an object pointer (i.e. our
 *     third operand), the conditional operator returns a "void *" type.
 *     Here, within the sizeof(), we would then get:
 *       sizeof(*((void *)(...)) == sizeof(void) == 1
 * - The equality comparison to "sizeof(int)" therefore depends on (x):
 *     sizeof(int) == sizeof(int)     (x) was a constant expression
 *     sizeof(int) != sizeof(void)    (x) was not a constant expression
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

/*
 * Useful shorthand for "is this condition known at compile-time?"
 *
 * Note that the condition may involve non-constant values,
 * but the compiler may know enough about the details of the
 * values to determine that the condition is statically true.
 */
#define statically_true(x) (__builtin_constant_p(x) && (x))

/*
 * Similar to statically_true() but produces a constant expression
 *
 * To be used in conjunction with macros, such as BUILD_BUG_ON_ZERO(),
 * which require their input to be a constant expression and for which
 * statically_true() would otherwise fail.
 *
 * This is a trade-off: const_true() requires all its operands to be
 * compile time constants. Else, it would always returns false even on
 * the most trivial cases like:
 *
 *   true || non_const_var
 *
 * On the opposite, statically_true() is able to fold more complex
 * tautologies and will return true on expressions such as:
 *
 *   !(non_const_var * 8 % 4)
 *
 * For the general case, statically_true() is better.
 */
#define const_true(x) __builtin_choose_expr(__is_constexpr(x), x, false)

/*
 * Memory Barriers
 * barrier(): Prevents the compiler from reordering instructions across this point.
 *            It does NOT prevent the CPU from reordering.
 */
#define cbarrier()      __asm__ volatile("" ::: "memory")
#define barrier()       cbarrier()

# define __compiletime_error(msg)       __attribute__((__error__(msg)))

# define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {								\
		/*							\
		 * __noreturn is needed to give the compiler enough	\
		 * information to avoid certain possibly-uninitialized	\
		 * warnings (regardless of the build failing).		\
		 */							\
		__noreturn extern void prefix ## suffix(void)		\
			__compiletime_error(msg);			\
		if (!(condition))					\
			prefix ## suffix();				\
	} while (0)

#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)

/**
 * compiletime_assert - break build and emit msg if condition is false
 * @condition: a compile-time constant condition to check
 * @msg:       a message to emit if condition is false
 *
 * In tradition of POSIX assert, this macro will break the build if the
 * supplied condition is *false*, emitting the supplied error message if the
 * compiler has support to do so.
 */
#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __COUNTER__)

#define compiletime_assert_atomic_type(t)				\
	compiletime_assert(__native_word(t),				\
		"Need native word sized stores/loads for atomicity.")

/* Is this type a native word size -- useful for atomic operations */
#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

/*
 * Yes, this permits 64-bit accesses on 32-bit architectures. These will
 * actually be atomic in some cases (namely Armv7 + LPAE), but for others we
 * rely on the access being split into 2x32-bit accesses for a 32-bit quantity
 * (e.g. a virtual address) and a strong prevailing wind.
 */
#define compiletime_assert_rwonce_type(t)					\
	compiletime_assert(__native_word(t) || sizeof(t) == sizeof(long long),	\
		"Unsupported access size for {READ,WRITE}_ONCE().")

/*
 * Use __READ_ONCE() instead of READ_ONCE() if you do not require any
 * atomicity. Note that this may result in tears!
 */
#ifndef __READ_ONCE
#define __READ_ONCE(x)	(*(const volatile __unqual_scalar_typeof(x) *)&(x))
#endif

#define READ_ONCE(x)							\
({									\
	compiletime_assert_rwonce_type(x);				\
	__READ_ONCE(x);							\
})

#define __WRITE_ONCE(x, val)						\
do {									\
	*(volatile typeof(x) *)&(x) = (val);				\
} while (0)

#define WRITE_ONCE(x, val)						\
do {									\
	compiletime_assert_rwonce_type(x);				\
	__WRITE_ONCE(x, val);						\
} while (0)

/* ========================
 * ALIGNMENT HELPERS
 * ======================== */

#define ALIGN(x, a)             (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)          ALIGN(x, a)

#define __STRINGIFY(x) #x
#define STRINGIFY(x) __STRINGIFY(x)

/* address spaces */
# define __kernel	__attribute__((address_space(0)))
# define __user		__attribute__((noderef))
# define __iomem	__attribute__((noderef))
# define __percpu
# define __rcu
static inline void __chk_user_ptr(const volatile void __user *ptr) { }
static inline void __chk_io_ptr(const volatile void __iomem *ptr) { }
/* other */
# define __preserve_most notrace __attribute__((__preserve_most__))
# define __force
# define __nocast	__attribute__((nocast))
# define __safe		__attribute__((safe))
# define __private	__attribute__((noderef))
# define ACCESS_PRIVATE(p, member) (*((typeof((p)->member) __force *) &(p)->member))

/*
 * Force a compilation error if condition is true, but also produce a
 * result (of value 0 and type int), so the expression can be used
 * e.g. in a structure initializer (or where-ever else comma expressions
 * aren't permitted).
 *
 * Take an error message as an optional second argument. If omitted,
 * default to the stringification of the tested expression.
 */
#define __BUILD_BUG_ON_ZERO_MSG(e, msg, ...) ((int)sizeof(struct {_Static_assert(!(e), msg);}))
#define BUILD_BUG_ON_ZERO(e, ...) \
	__BUILD_BUG_ON_ZERO_MSG(e, ##__VA_ARGS__, #e " is true")

/* Force a compilation error if a constant expression is not a power of 2 */
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)	\
	BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)			\
	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))

/**
 * BUILD_BUG_ON - break compile if a condition is true.
 * @condition: the condition which the compiler should know is false.
 *
 * If you have some code which relies on certain constants being equal, or
 * some other compile-time-evaluated condition, you should use BUILD_BUG_ON to
 * detect if someone changes it.
 */
#define BUILD_BUG_ON(condition) \
  BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)

/**
 * BUILD_BUG_ON_MSG - break compile if a condition is true & emit supplied
 *		      error message.
 * @condition: the condition which the compiler should know is false.
 *
 * See BUILD_BUG_ON for description.
 */
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

/*
 * BUILD_BUG_ON_INVALID() permits the compiler to check the validity of the
 * expression but avoids the generation of any code, even if that expression
 * has side-effects.
 */
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((__force long)(e))))

/*
 * Compile time check that field has an expected offset
 */
#define ASSERT_STRUCT_OFFSET(type, field, expected_offset)	\
	_Static_assert(offsetof(type, field) == (expected_offset),	\
		"Offset of " #field " in " #type " has changed.")

#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)

#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

#ifdef __clang__
#define ASM_INPUT_G "ir"
#define ASM_INPUT_RM "r"
#define ASM_OUTPUT_RM "=r"
#else
#define ASM_INPUT_G "g"
#define ASM_INPUT_RM "rm"
#define ASM_OUTPUT_RM "=rm"
#endif

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *			       non-scalar types unchanged.
 */
/*
 * Prefer C11 _Generic for better compile-times and simpler code. Note: 'char'
 * is not type-compatible with 'signed char', and we define a separate case.
 */
#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,			\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(char)0,				\
			 __scalar_type_to_expr_cases(char),		\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),		\
			 __scalar_type_to_expr_cases(long),		\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))

/*
 * __signed_scalar_typeof(x) - Declare a signed scalar type, leaving
 *			       non-scalar types unchanged.
 */

#define __scalar_type_to_signed_cases(type)				\
		unsigned type:	(signed type)0,				\
		signed type:	(signed type)0

#define __signed_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(signed char)0,				\
			 __scalar_type_to_signed_cases(char),		\
			 __scalar_type_to_signed_cases(short),		\
			 __scalar_type_to_signed_cases(int),		\
			 __scalar_type_to_signed_cases(long),		\
			 __scalar_type_to_signed_cases(long long),	\
			 default: (x)))

/* Helpers for emitting diagnostics in pragmas. */
#ifndef __diag
#define __diag(string)
#endif

#ifndef __diag_GCC
#define __diag_GCC(version, severity, string)
#endif

#define __diag_push()	__diag(push)
#define __diag_pop()	__diag(pop)

#define __diag_ignore(compiler, version, option, comment) \
__diag_ ## compiler(version, ignore, option)
#define __diag_warn(compiler, version, option, comment) \
__diag_ ## compiler(version, warn, option)
#define __diag_error(compiler, version, option, comment) \
__diag_ ## compiler(version, error, option)

#ifndef __diag_ignore_all
#define __diag_ignore_all(option, comment)
#endif

/*
 * sanitizers
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