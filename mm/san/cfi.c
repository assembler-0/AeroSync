/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/san/cfi.c
 * @brief Control Flow Integrity (CFI) failure handler
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */


#include <aerosync/compiler.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <lib/printk.h>
#include <aerosync/export.h>
#include <aerosync/stacktrace.h>

/**
 * CFI metadata structures based on Clang/LLVM implementation.
 */

struct CFICheckFailData {
  uint8_t check_kind;
  struct SourceLocation location;
  struct TypeDescriptor *type;
};

enum CFICheckKind {
  CFITCK_V_CALL,
  CFITCK_NV_CALL,
  CFITCK_DERIVED_CAST,
  CFITCK_UNRELATED_CAST,
  CFITCK_I_CALL,
};

static const char *const cfi_check_names[] = {
  "vcall",
  "nvcall",
  "derived_cast",
  "unrelated_cast",
  "icall",
};

/**
 * @brief CFI failure handler
 *
 * This function is called by the compiler-generated CFI checks when a
 * violation is detected. It logs the failure details and panics the kernel.
 *
 * @param ptr The address that failed the CFI check
 * @param type_data Compiler-provided data about the expected type (if available)
 *
 * Reentrancy: Not reentrant (protected by in_cfi flag)
 * Context: Any (can be called from interrupt context)
 * Locking: None (atomic-like flag used for recursion protection)
 */
static int in_cfi = 0;

static void print_location(const struct SourceLocation *loc) {
  if (loc && loc->file) {
    printk(KERN_EMERG CFI_CLASS "Location: %s:%d:%d\n", loc->file, loc->line,
           loc->column);
  } else {
    printk(KERN_EMERG CFI_CLASS "Location: unknown\n");
  }
}

static void print_type(const char *label, const struct TypeDescriptor *type) {
  if (type) {
    printk(KERN_EMERG CFI_CLASS "%s: %s\n", label, type->type_name);
  }
}

void __cfi_check_fail(void *ptr, void *type_data) {
  if (in_cfi++)
    goto out;

  printk(KERN_EMERG CFI_CLASS "Control Flow Integrity failure\n");

  if (type_data) {
    struct CFICheckFailData *data = (struct CFICheckFailData *) type_data;

    if (data->check_kind < sizeof(cfi_check_names) / sizeof(cfi_check_names[0])) {
      printk(KERN_EMERG CFI_CLASS "Check kind: %s\n", cfi_check_names[data->check_kind]);
    } else {
      printk(KERN_EMERG CFI_CLASS "Check kind: unknown (%d)\n", data->check_kind);
    }

    print_location(&data->location);
    print_type("Expected type", data->type);
  }

  printk(KERN_EMERG CFI_CLASS "Target address: %p\n", ptr);

  /* Provide a source trace for easier debugging */
  dump_stack();

  panic(CFI_CLASS "__cfi_check_fail");

out:
  in_cfi--;
}

EXPORT_SYMBOL(__cfi_check_fail);
