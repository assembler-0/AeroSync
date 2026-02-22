/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/acpi.c
 * @brief ACPI Table Parser Implementation using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/madt.h>
#include <aerosync/sysintf/dmar.h>
#include <lib/printk.h>

static ACPI_TABLE_FADT *s_fadt = nullptr;
static uint32_t s_waet_flags = 0;
static bool s_waet_present = false;

static struct acpi_mcfg_allocation *s_mcfg_entries = nullptr;
static size_t s_mcfg_count = 0;

static ACPI_TABLE_SPCR *s_spcr = nullptr;
static ACPI_TABLE_BGRT *s_bgrt = nullptr;
static ACPI_TABLE_HPET *s_hpet = nullptr;

/* --- Private Helpers --- */

static void parse_waet(void) {
  ACPI_TABLE_HEADER *tbl;
  ACPI_STATUS st = AcpiGetTable(ACPI_WAET_SIGNATURE, 1, &tbl);
  if (ACPI_FAILURE(st))
    return;

  struct acpi_waet *waet = (struct acpi_waet *) tbl;
  s_waet_flags = waet->flags;
  s_waet_present = true;

  printk(KERN_DEBUG ACPI_CLASS "WAET parsed: RTC good: %s, PM Timer good: %s\n",
         (s_waet_flags & ACPI_WAET_RTC_GOOD) ? "Yes" : "No",
         (s_waet_flags & ACPI_WAET_PM_TIMER_GOOD) ? "Yes" : "No");
}

static void parse_mcfg(void) {
  ACPI_TABLE_HEADER *tbl;
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_MCFG, 1, &tbl);
  if (ACPI_FAILURE(st))
    return;

  ACPI_TABLE_MCFG *mcfg = (ACPI_TABLE_MCFG *) tbl;
  size_t total_size = mcfg->Header.Length;
  size_t header_size = sizeof(ACPI_TABLE_MCFG);

  if (total_size > header_size) {
    s_mcfg_count =
        (total_size - header_size) / sizeof(ACPI_MCFG_ALLOCATION);
    s_mcfg_entries = (struct acpi_mcfg_allocation *) (mcfg + 1); // Entries follow the header

    printk(KERN_DEBUG ACPI_CLASS "MCFG found: %zu segments detected\n",
           s_mcfg_count);
    for (size_t i = 0; i < s_mcfg_count; i++) {
      printk(KERN_DEBUG ACPI_CLASS "  [%zu] base: 0x%llx, Bus: %d-%d\n", i,
             s_mcfg_entries[i].Address, s_mcfg_entries[i].StartBusNumber,
             s_mcfg_entries[i].EndBusNumber);
    }
  }
}

static void parse_spcr(void) {
  ACPI_TABLE_HEADER *tbl;
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_SPCR, 1, &tbl);
  if (ACPI_FAILURE(st))
    return;

  s_spcr = (ACPI_TABLE_SPCR *) tbl;
  printk(KERN_INFO ACPI_CLASS
         "SPCR found: Console on UART type %d, addr 0x%llx\n",
         s_spcr->InterfaceType, s_spcr->SerialPort.Address);
}

static void parse_bgrt(void) {
  ACPI_TABLE_HEADER *tbl;
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_BGRT, 1, &tbl);
  if (ACPI_FAILURE(st))
    return;

  s_bgrt = (ACPI_TABLE_BGRT *) tbl;
  printk(KERN_INFO ACPI_CLASS
         "BGRT found: Boot logo @ 0x%llx (type %d, version %d)\n",
         s_bgrt->ImageAddress, s_bgrt->ImageType, s_bgrt->Version);
}

static void parse_hpet(void) {
  ACPI_TABLE_HEADER *tbl;
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_HPET, 1, &tbl);
  if (ACPI_FAILURE(st))
    return;

  s_hpet = (ACPI_TABLE_HPET *) tbl;
  printk(KERN_INFO ACPI_CLASS "HPET table found: addr 0x%llx, period %u fs\n",
         s_hpet->Address.Address, s_hpet->MinimumTick);
}

static void parse_fadt(void) {
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_FADT, 1, (ACPI_TABLE_HEADER **)&s_fadt);
  if (ACPI_FAILURE(st)) {
    printk(KERN_WARNING ACPI_CLASS "failed to fetch FADT: %s\n",
           AcpiFormatException(st));
  } else {
    printk(KERN_INFO ACPI_CLASS "FADT version %d initialized\n",
           s_fadt->Header.Revision);
  }
}

/* --- Public API --- */

int acpi_tables_init(void) {
  // 1. FADT
  parse_fadt();

  // 2. WAET
  parse_waet();

  // 3. MCFG
  parse_mcfg();

  // 4. SPCR
  parse_spcr();

  // 5. BGRT
  parse_bgrt();

  // 6. HPET
  parse_hpet();

  // 7. MADT
  madt_init();

  // 8. DMAR
  dmar_init();

  return 0;
}

struct acpi_fadt *acpi_get_fadt(void) { return (struct acpi_fadt *)s_fadt; }

bool acpi_fadt_supports_reset_reg(void) {
  if (!s_fadt)
    return false;
  return (s_fadt->Header.Revision >= 2) && (s_fadt->Flags & ACPI_FADT_RESET_REGISTER);
}

bool acpi_waet_is_rtc_good(void) {
  return s_waet_present && (s_waet_flags & ACPI_WAET_RTC_GOOD);
}

bool acpi_waet_is_pm_timer_good(void) {
  return s_waet_present && (s_waet_flags & ACPI_WAET_PM_TIMER_GOOD);
}

const struct acpi_mcfg_allocation *acpi_get_mcfg_entries(size_t *out_count) {
  *out_count = s_mcfg_count;
  return s_mcfg_entries;
}

const struct acpi_spcr *acpi_get_spcr(void) { return (const struct acpi_spcr *)s_spcr; }
const struct acpi_bgrt *acpi_get_bgrt(void) { return (const struct acpi_bgrt *)s_bgrt; }
const struct acpi_hpet *acpi_get_hpet(void) { return (const struct acpi_hpet *)s_hpet; }

ACPI_STATUS acpica_find_table(const char *signature, ACPI_TABLE_HEADER **out_table) {
  return AcpiGetTable((ACPI_STRING)signature, 1, out_table);
}

#include <aerosync/export.h>
EXPORT_SYMBOL(acpi_tables_init);
EXPORT_SYMBOL(acpi_get_fadt);
EXPORT_SYMBOL(acpi_waet_is_rtc_good);
EXPORT_SYMBOL(acpi_waet_is_pm_timer_good);
EXPORT_SYMBOL(acpi_get_mcfg_entries);
EXPORT_SYMBOL(acpi_get_spcr);
EXPORT_SYMBOL(acpi_get_bgrt);
EXPORT_SYMBOL(acpi_get_hpet);
