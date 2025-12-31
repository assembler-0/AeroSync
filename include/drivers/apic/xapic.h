#pragma once

#include <kernel/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

// --- External Variables (defined in apic.c) ---
extern uacpi_u64 xapic_madt_lapic_override_phys;   // 0 if not provided
extern int xapic_madt_parsed;

// --- Constants ---
#define XAPIC_DELIVERY_MODE_FIXED        (0b000 << 8)
#define XAPIC_DELIVERY_MODE_LOWEST_PRIO  (0b001 << 8)
#define XAPIC_DELIVERY_MODE_SMI          (0b010 << 8)
#define XAPIC_DELIVERY_MODE_NMI          (0b011 << 8)
#define XAPIC_DELIVERY_MODE_INIT         (0b100 << 8)
#define XAPIC_DELIVERY_MODE_STARTUP      (0b101 << 8)
