#include <kernel/panic.h>
#include <kernel/classes.h>

#define UBSAN_HANDLER(name) \
    void __attribute__((noreturn)) name() { \
        panic(UBSAN_CLASS #name); \
    }

UBSAN_HANDLER(__ubsan_handle_add_overflow)
UBSAN_HANDLER(__ubsan_handle_sub_overflow)
UBSAN_HANDLER(__ubsan_handle_mul_overflow)
UBSAN_HANDLER(__ubsan_handle_divrem_overflow)
UBSAN_HANDLER(__ubsan_handle_negate_overflow)
UBSAN_HANDLER(__ubsan_handle_shift_out_of_bounds)
UBSAN_HANDLER(__ubsan_handle_load_invalid_value)
UBSAN_HANDLER(__ubsan_handle_out_of_bounds)
UBSAN_HANDLER(__ubsan_handle_type_mismatch)
UBSAN_HANDLER(__ubsan_handle_vla_bound_not_positive)
UBSAN_HANDLER(__ubsan_handle_nonnull_return)
UBSAN_HANDLER(__ubsan_handle_nonnull_arg)
UBSAN_HANDLER(__ubsan_handle_pointer_overflow)
UBSAN_HANDLER(__ubsan_handle_float_cast_overflow)
UBSAN_HANDLER(__ubsan_handle_float_cast_invalid_value)
UBSAN_HANDLER(__ubsan_handle_invalid_builtin)
UBSAN_HANDLER(__ubsan_handle_missing_return)
UBSAN_HANDLER(__ubsan_handle_implicit_conversion)
UBSAN_HANDLER(__ubsan_handle_type_mismatch_v1)
UBSAN_HANDLER(__ubsan_handle_builtin_unreachable)
UBSAN_HANDLER(__ubsan_handle_function_type_mismatch)
UBSAN_HANDLER(__ubsan_handle_nonnull_return_v1)
UBSAN_HANDLER(__ubsan_handle_nonnull_arg_v1)