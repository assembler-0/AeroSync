#pragma once
#include <aerosync/compiler.h>

// Initialize ACPI power management (Power Button, etc.)
int __must_check acpi_power_init(void);

// Perform a system shutdown (S5)
void acpi_shutdown(void);

// Perform a system reboot (RESET)
void acpi_reboot(void);
