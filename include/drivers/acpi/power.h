#pragma once

// Initialize ACPI power management (Power Button, etc.)
void acpi_power_init(void);

// Perform a system shutdown (S5)
void acpi_shutdown(void);

// Perform a system reboot (RESET)
void acpi_reboot(void);
