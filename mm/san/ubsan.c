/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/san/ubsan.c
 * @brief Undefined Behavior Sanitizer runtime handlers
 * @copyright (C) 2025-2026 assembler-0
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

#include <compiler.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <lib/printk.h>
#include <aerosync/fkx/fkx.h>

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

struct TypeMismatchData {
  struct SourceLocation location;
  struct TypeDescriptor *type;
  uintptr_t alignment;
  unsigned char type_check_kind;
};

struct OverflowData {
  struct SourceLocation location;
  struct TypeDescriptor *type;
};

struct OutOfBoundsData {
  struct SourceLocation location;
  struct TypeDescriptor *array_type;
  struct TypeDescriptor *index_type;
};

struct ShiftOutOfBoundsData {
  struct SourceLocation location;
  struct TypeDescriptor *lhs_type;
  struct TypeDescriptor *rhs_type;
};

struct UnreachableData {
  struct SourceLocation location;
};

struct InvalidValueData {
  struct SourceLocation location;
  struct TypeDescriptor *type;
};

struct FloatCastOverflowData {
  struct SourceLocation location;
  struct TypeDescriptor *from_type;
  struct TypeDescriptor *to_type;
};

struct NonNullReturnData {
  struct SourceLocation location;
};

struct NonNullArgData {
  struct SourceLocation location;
};

static void print_location(const struct SourceLocation *loc) {
  if (loc) {
    printk(KERN_EMERG UBSAN_CLASS "Location: %s:%d:%d\n", loc->file, loc->line,
           loc->column);
  } else {
    printk(KERN_EMERG UBSAN_CLASS "Location: unknown\n");
  }
}

static void print_type(const char *label, const struct TypeDescriptor *type) {
  if (type) {
    printk(KERN_EMERG UBSAN_CLASS "%s: %s\n", label, type->type_name);
  }
}

// Handler helpers
#define UBSAN_HANDLER(name) void __noinline name

UBSAN_HANDLER(__ubsan_handle_add_overflow)(struct OverflowData *data,
                                           uintptr_t lhs, uintptr_t rhs) {
  printk(KERN_EMERG UBSAN_CLASS "Integer addition overflow\n");
  print_location(&data->location);
  print_type("Type", data->type);
  panic(UBSAN_CLASS "add_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_add_overflow);

UBSAN_HANDLER(__ubsan_handle_sub_overflow)(struct OverflowData *data,
                                           uintptr_t lhs, uintptr_t rhs) {
  printk(KERN_EMERG UBSAN_CLASS "Integer subtraction overflow\n");
  print_location(&data->location);
  print_type("Type", data->type);
  panic(UBSAN_CLASS "sub_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_sub_overflow);

UBSAN_HANDLER(__ubsan_handle_mul_overflow)(struct OverflowData *data,
                                           uintptr_t lhs, uintptr_t rhs) {
  printk(KERN_EMERG UBSAN_CLASS "Integer multiplication overflow\n");
  print_location(&data->location);
  print_type("Type", data->type);
  panic(UBSAN_CLASS "mul_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_mul_overflow);

UBSAN_HANDLER(__ubsan_handle_divrem_overflow)(struct OverflowData *data,
                                              uintptr_t lhs, uintptr_t rhs) {
  printk(KERN_EMERG UBSAN_CLASS "Integer division overflow\n");
  print_location(&data->location);
  print_type("Type", data->type);
  panic(UBSAN_CLASS "divrem_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_divrem_overflow);

UBSAN_HANDLER(__ubsan_handle_negate_overflow)(struct OverflowData *data,
                                              uintptr_t old_val) {
  printk(KERN_EMERG UBSAN_CLASS "Integer negation overflow\n");
  print_location(&data->location);
  print_type("Type", data->type);
  panic(UBSAN_CLASS "negate_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_negate_overflow);

UBSAN_HANDLER(__ubsan_handle_pointer_overflow)(struct OverflowData *data,
                                               uintptr_t base,
                                               uintptr_t result) {
  printk(KERN_EMERG UBSAN_CLASS "Pointer arithmetic overflow\n");
  print_location(&data->location);
  panic(UBSAN_CLASS "pointer_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_pointer_overflow);

UBSAN_HANDLER(__ubsan_handle_shift_out_of_bounds)(
    struct ShiftOutOfBoundsData *data, uintptr_t lhs, uintptr_t rhs) {
  printk(KERN_EMERG UBSAN_CLASS "Shift out of bounds\n");
  print_location(&data->location);
  print_type("LHS Type", data->lhs_type);
  print_type("RHS Type", data->rhs_type);
  panic(UBSAN_CLASS "shift_out_of_bounds");
}
EXPORT_SYMBOL(__ubsan_handle_shift_out_of_bounds);

UBSAN_HANDLER(__ubsan_handle_out_of_bounds)(struct OutOfBoundsData *data,
                                            uintptr_t index) {
  printk(KERN_EMERG UBSAN_CLASS "Out of bounds access\n");
  print_location(&data->location);
  print_type("Array Type", data->array_type);
  print_type("Index Type", data->index_type);
  printk(KERN_EMERG UBSAN_CLASS "Index: %lx\n", index);
  panic(UBSAN_CLASS "out_of_bounds");
}
EXPORT_SYMBOL(__ubsan_handle_out_of_bounds);

static const char * const type_check_kinds[] = {"load of",
                                         "store to",
                                         "reference binding to",
                                         "member access within",
                                         "member call on",
                                         "constructor call on",
                                         "downcast of",
                                         "downcast of",
                                         "upcast of",
                                         "cast to virtual base of",
                                         "_Nonnull binding to",
                                         "dynamic operation on"};

UBSAN_HANDLER(__ubsan_handle_type_mismatch)(struct TypeMismatchData *data,
                                            uintptr_t ptr) {
  if (!ptr) {
    printk(KERN_EMERG UBSAN_CLASS "nullptr pointer dereference\n");
  } else if (data->alignment && (ptr & (data->alignment - 1))) {
    printk(KERN_EMERG UBSAN_CLASS "Misaligned access\n");
    printk(KERN_EMERG UBSAN_CLASS "Address: 0x%lx (Alignment required: %ld)\n",
           ptr, data->alignment);
  } else {
    printk(KERN_EMERG UBSAN_CLASS "Type mismatch\n");
    printk(KERN_EMERG UBSAN_CLASS "Address: 0x%lx\n", ptr);
  }

  print_location(&data->location);
  print_type("Type", data->type);
  if (data->type_check_kind <
      sizeof(type_check_kinds) / sizeof(type_check_kinds[0])) {
    printk(KERN_EMERG UBSAN_CLASS "Operation: %s\n",
           type_check_kinds[data->type_check_kind]);
  }

  panic(UBSAN_CLASS "type_mismatch");
}
EXPORT_SYMBOL(__ubsan_handle_type_mismatch);

struct TypeMismatchDataV1 {
  struct SourceLocation location;
  struct TypeDescriptor *type;
  unsigned char log_alignment;
  unsigned char type_check_kind;
};

UBSAN_HANDLER(__ubsan_handle_type_mismatch_v1)(struct TypeMismatchDataV1 *data,
                                               uintptr_t ptr) {
  struct TypeMismatchData converted = {
    .location = data->location,
    .type = data->type,
    .alignment = 1UL << data->log_alignment,
    .type_check_kind = data->type_check_kind
  };
  __ubsan_handle_type_mismatch(&converted, ptr);
}
EXPORT_SYMBOL(__ubsan_handle_type_mismatch_v1);

UBSAN_HANDLER(__ubsan_handle_load_invalid_value)(struct InvalidValueData *data,
                                                 uintptr_t val) {
  printk(KERN_EMERG UBSAN_CLASS "Load of invalid value\n");
  print_location(&data->location);
  print_type("Type", data->type);
  printk(KERN_EMERG UBSAN_CLASS "Value: %lx\n", val);
  panic(UBSAN_CLASS "load_invalid_value");
}
EXPORT_SYMBOL(__ubsan_handle_load_invalid_value);

UBSAN_HANDLER(__ubsan_handle_builtin_unreachable)(
    struct UnreachableData *data) {
  printk(KERN_EMERG UBSAN_CLASS "Execution reached __builtin_unreachable()\n");
  if (data)
    print_location(&data->location);
  panic(UBSAN_CLASS "builtin_unreachable");
}
EXPORT_SYMBOL(__ubsan_handle_builtin_unreachable);

UBSAN_HANDLER(__ubsan_handle_missing_return)(struct UnreachableData *data) {
  printk(KERN_EMERG UBSAN_CLASS "Function missing return statement\n");
  if (data)
    print_location(&data->location);
  panic(UBSAN_CLASS "missing_return");
}
EXPORT_SYMBOL(__ubsan_handle_missing_return);

UBSAN_HANDLER(__ubsan_handle_vla_bound_not_positive)(
    struct UnreachableData *data, uintptr_t bound) {
  /* Using UnreachableData as it matches layout for simple location-only */
  printk(KERN_EMERG UBSAN_CLASS "VLA bound not positive\n");
  if (data)
    print_location(&data->location);
  printk(KERN_EMERG UBSAN_CLASS "Bound: %ld\n", (long)bound);
  panic(UBSAN_CLASS "vla_bound_not_positive");
}
EXPORT_SYMBOL(__ubsan_handle_vla_bound_not_positive);

UBSAN_HANDLER(__ubsan_handle_float_cast_overflow)(
    struct FloatCastOverflowData *data, uintptr_t from) {
  printk(KERN_EMERG UBSAN_CLASS "Float cast overflow\n");
  print_location(&data->location);
  print_type("From", data->from_type);
  print_type("To", data->to_type);
  panic(UBSAN_CLASS "float_cast_overflow");
}
EXPORT_SYMBOL(__ubsan_handle_float_cast_overflow);

UBSAN_HANDLER(__ubsan_handle_nonnull_return_v1)(struct NonNullReturnData *data,
                                                struct SourceLocation *loc) {
  printk(KERN_EMERG UBSAN_CLASS "Nonnull return value is null\n");
  print_location(&data->location); // Data contains location in newer versions?
                                   // Or loc used?
  /* Note: Calling convention for nonnull_return varies, sometimes it has 2
     args. Usually: (NonNullReturnData *data, SourceLocation *loc) or just data.
     We'll print data->location. */
  panic(UBSAN_CLASS "nonnull_return");
}
EXPORT_SYMBOL(__ubsan_handle_nonnull_return_v1);

UBSAN_HANDLER(__ubsan_handle_nonnull_arg)(struct NonNullArgData *data) {
  printk(KERN_EMERG UBSAN_CLASS "Nonnull argument is null\n");
  print_location(&data->location);
  panic(UBSAN_CLASS "nonnull_arg");
}
EXPORT_SYMBOL(__ubsan_handle_nonnull_arg);

UBSAN_HANDLER(__ubsan_handle_nonnull_arg_v1)(struct NonNullArgData *data) {
  __ubsan_handle_nonnull_arg(data);
}
EXPORT_SYMBOL(__ubsan_handle_nonnull_arg_v1);

struct ImplicitConversionData {
  struct SourceLocation location;
  struct TypeDescriptor *from_type;
  struct TypeDescriptor *to_type;
  unsigned char kind;
};

UBSAN_HANDLER(__ubsan_handle_implicit_conversion)(
    struct ImplicitConversionData *data, uintptr_t src, uintptr_t dst) {
  printk(KERN_EMERG UBSAN_CLASS "Implicit conversion issue\n");
  print_location(&data->location);
  print_type("From", data->from_type);
  print_type("To", data->to_type);
  panic(UBSAN_CLASS "implicit_conversion");
}
EXPORT_SYMBOL(__ubsan_handle_implicit_conversion);

struct FunctionTypeMismatchData {
  struct SourceLocation location;
  struct TypeDescriptor *type;
};

UBSAN_HANDLER(__ubsan_handle_function_type_mismatch)(
    struct FunctionTypeMismatchData *data, uintptr_t ptr) {
  printk(KERN_EMERG UBSAN_CLASS "Function type mismatch\n");
  print_location(&data->location);
  print_type("Type", data->type);
  printk(KERN_EMERG UBSAN_CLASS "Address: %lx\n", ptr);
  panic(UBSAN_CLASS "function_type_mismatch");
}
EXPORT_SYMBOL(__ubsan_handle_function_type_mismatch);

struct InvalidBuiltinData {
  struct SourceLocation location;
  unsigned char kind;
};

UBSAN_HANDLER(__ubsan_handle_invalid_builtin)(struct InvalidBuiltinData *data) {
  printk(KERN_EMERG UBSAN_CLASS "Invalid builtin usage\n");
  print_location(&data->location);
  panic(UBSAN_CLASS "invalid_builtin");
}
EXPORT_SYMBOL(__ubsan_handle_invalid_builtin);

UBSAN_HANDLER(__ubsan_handle_float_cast_invalid_value)(
    struct FloatCastOverflowData *data, uintptr_t val) {
  printk(KERN_EMERG UBSAN_CLASS "Float cast invalid value\n");
  print_location(&data->location);
  print_type("From", data->from_type);
  print_type("To", data->to_type);
  panic(UBSAN_CLASS "float_cast_invalid_value");
}
EXPORT_SYMBOL(__ubsan_handle_float_cast_invalid_value);