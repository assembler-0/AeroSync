#include <arch/x64/cpu.h>
#include <arch/x64/gdt/gdt.h>
#include <compiler.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kernel/panic.h>

// GDT with 7 entries: null, kcode, kdata, ucode, udata, tss_low, tss_high
// CRITICAL: GDT must be properly aligned for x86-64
static struct gdt_entry gdt[7] __aligned(16);
static struct gdt_ptr gdt_ptr __aligned(16);
static struct tss_entry tss __aligned(16);

extern void gdt_flush(const struct gdt_ptr *gdt_ptr_addr);
extern void tss_flush(void);

static volatile int gdt_lock = 0;

static void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  irq_flags_t flags = spinlock_lock_irqsave(&gdt_lock);
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;

  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = (limit >> 16) & 0x0F;
  gdt[num].granularity |= gran & 0xF0;
  gdt[num].access = access;
  spinlock_unlock_irqrestore(&gdt_lock, flags);
}

// A cleaner way to set up the 64-bit TSS descriptor
static void set_tss_gate(int num, uint64_t base, uint64_t limit) {
  irq_flags_t flags = spinlock_lock_irqsave(&gdt_lock);
  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].access = GDT_ACCESS_TSS; // Access byte for 64-bit TSS
  gdt[num].granularity =
      (limit >> 16) & 0x0F; // No granularity bits (G=0, AVL=0)
  gdt[num].base_high = (base >> 24) & 0xFF;

  uint32_t *base_high_ptr = (uint32_t *)&gdt[num + 1];
  *base_high_ptr = (base >> 32);

  *((uint32_t *)base_high_ptr + 1) = 0;
  spinlock_unlock_irqrestore(&gdt_lock, flags);
}

void gdt_init(void) {
  printk(GDT_CLASS "Initializing GDT\n");
  gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
  gdt_ptr.base = (uint64_t)&gdt;

  set_gdt_gate(0, 0, 0, 0, 0); // 0x00: Null segment
  set_gdt_gate(1, 0, 0xFFFFFFFF, GDT_ACCESS_CODE_PL0,
               GDT_GRAN_CODE); // 0x08: Kernel Code
  set_gdt_gate(2, 0, 0xFFFFFFFF, GDT_ACCESS_DATA_PL0,
               GDT_GRAN_DATA); // 0x10: Kernel Data
  set_gdt_gate(3, 0, 0xFFFFFFFF, GDT_ACCESS_CODE_PL3,
               GDT_GRAN_CODE); // 0x18: User Code
  set_gdt_gate(4, 0, 0xFFFFFFFF, GDT_ACCESS_DATA_PL3,
               GDT_GRAN_DATA); // 0x20: User Data

  uint64_t tss_base = (uint64_t)&tss;
  uint64_t tss_limit = sizeof(struct tss_entry) - 1;
  set_tss_gate(5, tss_base, tss_limit);

  tss.iomap_base = sizeof(struct tss_entry);

  gdt_flush(&gdt_ptr);
  tss_flush();
  printk(GDT_CLASS "GDT initialized\n");
}

void gdt_init_ap(void) {
    // APs are initialized after slab_init, so we can use kmalloc
    printk(GDT_CLASS "Initializing GDT for AP\n");
    struct gdt_entry *ap_gdt = kmalloc(sizeof(struct gdt_entry) * 7);
    struct tss_entry *ap_tss = kmalloc(sizeof(struct tss_entry));

    if (!ap_gdt || !ap_tss) {
        panic(GDT_CLASS "Failed to allocate GDT/TSS for AP");
    }

    // Initialize TSS
    memset(ap_tss, 0, sizeof(struct tss_entry));
    ap_tss->iomap_base = sizeof(struct tss_entry);

    // Copy GDT entries 0-4 (Null, KCode, KData, UCode, UData) from BSP
    // These segments are identical for all CPUs
    memcpy(ap_gdt, gdt, sizeof(struct gdt_entry) * 5);

    // Setup TSS descriptor in GDT (index 5 and 6)
    uint64_t base = (uint64_t)ap_tss;
    uint64_t limit = sizeof(struct tss_entry) - 1;

    ap_gdt[5].limit_low = (limit & 0xFFFF);
    ap_gdt[5].base_low = (base & 0xFFFF);
    ap_gdt[5].base_middle = (base >> 16) & 0xFF;
    ap_gdt[5].access = GDT_ACCESS_TSS;
    ap_gdt[5].granularity = (limit >> 16) & 0x0F;
    ap_gdt[5].base_high = (base >> 24) & 0xFF;

    uint32_t *base_high_ptr = (uint32_t *)&ap_gdt[6];
    *base_high_ptr = (base >> 32);
    *((uint32_t *)base_high_ptr + 1) = 0;

    // Load GDT
    struct gdt_ptr ap_gdt_ptr;
    ap_gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
    ap_gdt_ptr.base = (uint64_t)ap_gdt;

    gdt_flush(&ap_gdt_ptr);
    tss_flush();
    
    // Note: We might want to save ap_gdt and ap_tss to a per-cpu structure later
    // but for now this prevents the triple fault.
}

void set_tss_rsp0(uint64_t rsp0) { tss.rsp0 = rsp0; }