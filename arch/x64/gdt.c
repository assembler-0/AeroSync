#include <arch/x64/gdt.h>
#include <compiler.h>
#include <kernel/types.h>

// GDT entry (legacy format). In x86_64, base/limit largely ignored for flat segments.
typedef struct __packed {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;     // type | S | DPL | P
    uint8_t  gran;       // limit_high | AVL | L | D | G
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __packed {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

// We keep it flexible: small static GDT that can be extended later if needed.
static gdt_entry_t gdt[6];
static gdtr_t gdtr;

// Helper to encode a descriptor.
static void set_seg_descriptor(gdt_entry_t* e, uint32_t base, uint32_t limit,
                               uint8_t access, uint8_t flags)
{
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->base_low  = (uint16_t)(base & 0xFFFF);
    e->base_mid  = (uint8_t)((base >> 16) & 0xFF);
    e->access    = access;
    e->gran      = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
}

// Access byte bits
#define ACC_P  0x80
#define ACC_DPL(n) ((n) << 5)
#define ACC_S  0x10
#define ACC_TYPE_CODE 0x08
#define ACC_TYPE_DATA 0x00
#define ACC_TYPE_RW   0x02
#define ACC_TYPE_EXEC 0x08

// Flags
#define FLG_G  0x80  // granularity 4K
#define FLG_D  0x40  // default operand size (0 for 64-bit code)
#define FLG_L  0x20  // 64-bit code segment
#define FLG_AVL 0x10

// Load segment selectors. CS requires a far jump, use assembly sequence.
static inline void load_segments(uint16_t cs, uint16_t ds)
{
    // Load data segments first
    __asm__ volatile (
        "mov %0, %%ds\n\t"
        "mov %0, %%es\n\t"
        "mov %0, %%ss\n\t"
        : : "r"(ds) : "memory");

    // Far return / jump to reload CS
    // We use pushq cs; pushq rip; lretq trick
    uint64_t rip;
    // label trick inside asm is more robust
    __asm__ volatile (
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %0\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : "r"((uint64_t)cs) : "rax", "memory");
    (void)rip;
}

void gdt_init(void)
{
    // Null descriptor
    set_seg_descriptor(&gdt[GDT_NULL_INDEX], 0, 0, 0, 0);

    // Flat Kernel Code: Present, DPL=0, Code|RW, Long mode (L=1), G=1, D=0
    set_seg_descriptor(&gdt[GDT_KERNEL_CODE_INDEX], 0, 0x000FFFFF,
                       ACC_P | ACC_DPL(0) | ACC_S | ACC_TYPE_EXEC | ACC_TYPE_RW,
                       FLG_G | FLG_L);

    // Flat Kernel Data: Present, DPL=0, Data|RW, L=0, G=1, D=1 (data/legacy)
    set_seg_descriptor(&gdt[GDT_KERNEL_DATA_INDEX], 0, 0x000FFFFF,
                       ACC_P | ACC_DPL(0) | ACC_S | ACC_TYPE_DATA | ACC_TYPE_RW,
                       FLG_G | FLG_D);

    // Flat User Code: Present, DPL=3, Code|RW, Long mode
    set_seg_descriptor(&gdt[GDT_USER_CODE_INDEX], 0, 0x000FFFFF,
                       ACC_P | ACC_DPL(3) | ACC_S | ACC_TYPE_EXEC | ACC_TYPE_RW,
                       FLG_G | FLG_L);

    // Flat User Data: Present, DPL=3, Data|RW
    set_seg_descriptor(&gdt[GDT_USER_DATA_INDEX], 0, 0x000FFFFF,
                       ACC_P | ACC_DPL(3) | ACC_S | ACC_TYPE_DATA | ACC_TYPE_RW,
                       FLG_G | FLG_D);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)&gdt[0];

    // Load GDT
    __asm__ volatile ("lgdt %0" : : "m"(gdtr));

    // Reload segments (CS via far return, data via moves)
    load_segments(GDT_KERNEL_CODE_SEL, GDT_KERNEL_DATA_SEL);
}
