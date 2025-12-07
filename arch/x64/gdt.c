#include <arch/x64/cpu.h>
#include <arch/x64/gdt.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <lib/printk.h>

// GDT with 7 entries: null, kcode, kdata, ucode, udata, tss_low, tss_high
// CRITICAL: GDT must be properly aligned for x86-64
static struct gdt_entry gdt[7] __attribute__((aligned(16)));
static struct gdt_ptr gdt_ptr __attribute__((aligned(16)));
static struct tss_entry tss __attribute__((aligned(16)));

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

void set_tss_rsp0(uint64_t rsp0) { tss.rsp0 = rsp0; }