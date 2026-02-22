#pragma once

#include <aerosync/types.h>
#include <aerosync/compiler.h>

/**
 * @brief Initialize ACPICA OSL and subsystem early.
 * Should be called before any ACPI tables are accessed.
 */
int __must_check acpica_kernel_init_early(void);

/**
 * @brief Initialize ACPICA namespace and late subsystem.
 * Should be called after interrupt controller is ready.
 */
int __must_check acpica_kernel_init_late(void);

/**
 * @brief Notify ACPICA that the interrupt controller is ready.
 */
void acpica_notify_ic_ready(void);
