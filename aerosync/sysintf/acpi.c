/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/acpi.c
 * @brief Advanced ACPI Table Parser Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/madt.h>
#include <lib/printk.h>

static struct acpi_fadt *s_fadt = NULL;
static uacpi_u32 s_waet_flags = 0;
static bool s_waet_present = false;

static struct acpi_mcfg_allocation *s_mcfg_entries = NULL;
static size_t s_mcfg_count = 0;

static struct acpi_spcr *s_spcr = NULL;
static struct acpi_bgrt *s_bgrt = NULL;
static struct acpi_hpet *s_hpet = NULL;

/* --- Private Helpers --- */

static void parse_waet(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_WAET_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st))
    return;

  struct acpi_waet *waet = (struct acpi_waet *) tbl.ptr;
  s_waet_flags = waet->flags;
  s_waet_present = true;

  printk(KERN_DEBUG ACPI_CLASS "WAET parsed: RTC good: %s, PM Timer good: %s\n",
         (s_waet_flags & ACPI_WAET_RTC_GOOD) ? "Yes" : "No",
         (s_waet_flags & ACPI_WAET_PM_TIMER_GOOD) ? "Yes" : "No");

  uacpi_table_unref(&tbl);
}

static void parse_mcfg(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st))
    return;

  struct acpi_mcfg *mcfg = (struct acpi_mcfg *) tbl.ptr;
  size_t total_size = mcfg->hdr.length;
  size_t header_size = sizeof(struct acpi_mcfg);

  if (total_size > header_size) {
    s_mcfg_count =
        (total_size - header_size) / sizeof(struct acpi_mcfg_allocation);
    s_mcfg_entries = (struct acpi_mcfg_allocation *) (mcfg->entries);

    printk(KERN_DEBUG ACPI_CLASS "MCFG found: %zu segments detected\n",
           s_mcfg_count);
    for (size_t i = 0; i < s_mcfg_count; i++) {
      printk(KERN_DEBUG ACPI_CLASS "  [%zu] base: 0x%llx, Bus: %d-%d\n", i,
             s_mcfg_entries[i].address, s_mcfg_entries[i].start_bus,
             s_mcfg_entries[i].end_bus);
    }
  }

  /* NOTE: Table is mapped in memory. If we unref, it might get unmapped. */
}

static void parse_spcr(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_SPCR_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st))
    return;

  s_spcr = (struct acpi_spcr *) tbl.ptr;
  printk(KERN_INFO ACPI_CLASS
         "SPCR found: Console on UART type %d, addr 0x%llx\n",
         s_spcr->interface_type, s_spcr->base_address.address);
}

static void parse_bgrt(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_BGRT_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st))
    return;

  s_bgrt = (struct acpi_bgrt *) tbl.ptr;
  printk(KERN_INFO ACPI_CLASS
         "BGRT found: Boot logo @ 0x%llx (type %d, version %d)\n",
         s_bgrt->image_address, s_bgrt->image_type, s_bgrt->version);
}

static void parse_hpet(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st))
    return;

  s_hpet = (struct acpi_hpet *) tbl.ptr;
  printk(KERN_INFO ACPI_CLASS "HPET table found: addr 0x%llx, period %u fs\n",
         s_hpet->address.address, s_hpet->min_clock_tick);
}

/* --- Public API --- */

int acpi_tables_init(void) {
  uacpi_status st;

  // 1. FADT
  st = uacpi_table_fadt(&s_fadt);
  if (uacpi_unlikely_error(st)) {
    printk(KERN_WARNING ACPI_CLASS "failed to fetch FADT: %s\n",
           uacpi_status_to_string(st));
  } else {
    printk(KERN_INFO ACPI_CLASS "FADT version %d initialized\n",
           s_fadt->hdr.revision);
  }

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

  // 7. MADT (Integration)
  madt_init();

  return 0;
}

struct acpi_fadt *acpi_get_fadt(void) { return s_fadt; }

bool acpi_fadt_supports_reset_reg(void) {
  if (!s_fadt)
    return false;
  return (s_fadt->hdr.revision >= 2) && (s_fadt->flags & (1 << 10));
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

const struct acpi_spcr *acpi_get_spcr(void) { return s_spcr; }
const struct acpi_bgrt *acpi_get_bgrt(void) { return s_bgrt; }
const struct acpi_hpet *acpi_get_hpet(void) { return s_hpet; }

uacpi_status acpi_find_table(const char *signature, uacpi_table *out_table) {
  return uacpi_table_find_by_signature(signature, out_table);
}

void acpi_unref_table(uacpi_table *tbl) { uacpi_table_unref(tbl); }

EXPORT_SYMBOL(acpi_tables_init);
EXPORT_SYMBOL(acpi_get_fadt);
EXPORT_SYMBOL(acpi_waet_is_rtc_good);
EXPORT_SYMBOL(acpi_waet_is_pm_timer_good);
EXPORT_SYMBOL(acpi_get_mcfg_entries);
EXPORT_SYMBOL(acpi_get_spcr);
EXPORT_SYMBOL(acpi_get_bgrt);
EXPORT_SYMBOL(acpi_get_hpet);
EXPORT_SYMBOL(acpi_find_table);
EXPORT_SYMBOL(acpi_unref_table);
