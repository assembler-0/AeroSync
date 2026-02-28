#ifndef __ACAEROSYNC_H__
#define __ACAEROSYNC_H__

#include <aerosync/types.h>
#include <lib/string.h>
#include <aerosync/ctype.h>

#undef ACPI_USE_LOCAL_CACHE

#define ACPI_USE_SYSTEM_CLIBRARY        1

#define ACPI_MACHINE_WIDTH	            64

#define COMPILER_DEPENDENT_INT64        int64_t
#define COMPILER_DEPENDENT_UINT64       uint64_t

#define ACPI_UINTPTR_T                  uintptr_t

#define ACPI_USE_DO_WHILE_0

#define USE_NATIVE_ALLOCATE_ZEROED

#undef  ACPI_DEBUGGER
#undef  ACPI_DISASSEMBLER

#define ACPI_MUTEX_TYPE                 ACPI_OSL_MUTEX

#define ACPI_OFFSET(d, f)               __builtin_offsetof(d, f)

#define ACPI_LIBRARY

#include <platform/acgcc.h>

#endif
