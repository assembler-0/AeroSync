#include <arch/x64/cpu.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <drivers/apic/apic.h>
#include <arch/x64/io.h>
#include <kernel/classes.h>
#include <lib/printk.h>

// --- Register Definitions ---

// Local APIC registers (offsets from LAPIC base)
#define LAPIC_ID 0x0020               // LAPIC ID
#define LAPIC_VER 0x0030              // LAPIC Version
#define LAPIC_TPR 0x0080              // Task Priority
#define LAPIC_EOI 0x00B0              // EOI
#define LAPIC_LDR 0x00D0              // Logical Destination
#define LAPIC_DFR 0x00E0              // Destination Format
#define LAPIC_SVR 0x00F0              // Spurious Interrupt Vector
#define LAPIC_ESR 0x0280              // Error Status
#define LAPIC_ICR_LOW 0x0300          // Interrupt Command Reg low
#define LAPIC_ICR_HIGH 0x0310         // Interrupt Command Reg high
#define LAPIC_LVT_TIMER 0x0320        // LVT Timer
#define LAPIC_LVT_LINT0 0x0350        // LVT LINT0
#define LAPIC_LVT_LINT1 0x0360        // LVT LINT1
#define LAPIC_LVT_ERROR 0x0370        // LVT Error
#define LAPIC_TIMER_INIT_COUNT 0x0380 // Initial Count (for Timer)
#define LAPIC_TIMER_CUR_COUNT 0x0390  // Current Count (for Timer)
#define LAPIC_TIMER_DIV 0x03E0        // Divide Configuration

// I/O APIC registers
#define IOAPIC_REG_ID 0x00    // ID Register
#define IOAPIC_REG_VER 0x01   // Version Register
#define IOAPIC_REG_TABLE 0x10 // Redirection Table

// --- Constants ---
#define APIC_BASE_MSR 0x1B
#define APIC_BASE_MSR_ENABLE 0x800

#define IOAPIC_DEFAULT_PHYS_ADDR 0xFEC00000

// --- Globals ---
// Mapped addresses
volatile uint32_t *g_lapic_base = NULL;
volatile uint32_t *g_ioapic_base = NULL;
volatile uint32_t g_apic_timer_hz = 0;

// --- Forward Declarations ---
static void lapic_write(uint32_t reg, uint32_t value);
static uint32_t lapic_read(uint32_t reg);
static void ioapic_write(uint8_t reg, uint32_t value);
static uint32_t ioapic_read(uint8_t reg);
static void ioapic_set_entry(uint8_t index, uint64_t data);
static bool detect_apic(void);

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

static uint16_t s_irq_mask = 0xFFFF; // All masked initially

// Helper to write the cached mask to the PICs
static void pic_write_mask(void) {
  outb(PIC1_DATA, s_irq_mask & 0xFF);
  outb(PIC2_DATA, (s_irq_mask >> 8) & 0xFF);
}

void pic_mask_all(void) {
  s_irq_mask = 0xFFFF;
  pic_write_mask();
}

// --- MMIO Helper Functions ---

static void lapic_write(uint32_t reg, uint32_t value) {
  if (g_lapic_base)
    g_lapic_base[reg / 4] = value;
}

static uint32_t lapic_read(uint32_t reg) {
  if (!g_lapic_base)
    return 0;
  return g_lapic_base[reg / 4];
}

uint8_t lapic_get_id(void) {
  // LAPIC_ID register: bits 24..31 hold the APIC ID in xAPIC mode
  return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

static void ioapic_write(uint8_t reg, uint32_t value) {
  if (!g_ioapic_base)
    return;
  // I/O APIC uses an index/data pair for access
  g_ioapic_base[0] = reg;
  g_ioapic_base[4] = value;
}

static uint32_t ioapic_read(uint8_t reg) {
  if (!g_ioapic_base)
    return 0;
  g_ioapic_base[0] = reg;
  return g_ioapic_base[4];
}

// Sets a redirection table entry in the I/O APIC
static void ioapic_set_entry(uint8_t index, uint64_t data) {
  ioapic_write(IOAPIC_REG_TABLE + index * 2, (uint32_t)data);
  ioapic_write(IOAPIC_REG_TABLE + index * 2 + 1, (uint32_t)(data >> 32));
}

// --- Core APIC Functions ---

// Main entry point to initialize the APIC system
bool apic_init(void) {
  if (!detect_apic()) {
    printk(KERN_ERR APIC_CLASS "No local APIC found or supported.\n");
    return false;
  }

  pic_mask_all();

  if (!setup_lapic()) {
    printk(KERN_ERR APIC_CLASS "Failed to setup Local APIC.\n");
    return false;
  }

  if (!setup_ioapic()) {
    printk(KERN_ERR APIC_CLASS "Failed to setup I/O APIC.\n");
    return false;
  }

  return true;
}

void apic_send_eoi(void) { lapic_write(LAPIC_EOI, 0); }

// --- I/O APIC Interrupt Management ---

void apic_enable_irq(uint8_t irq_line) {
  // IRQ line -> Vector 32 + IRQ
  // Route to the current CPU's LAPIC ID (BSP)
  uint8_t dest_apic_id = lapic_get_id();

  uint64_t redirect_entry = (32 + irq_line); // Vector
  redirect_entry |= (0b000ull << 8);         // Delivery Mode: Fixed
  redirect_entry |= (0b0ull << 11);          // Destination Mode: Physical
  redirect_entry |= (0b0ull << 13);          // Trigger Mode: Edge
  redirect_entry |= (0b0ull << 15);          // Polarity: High (active high)
  // Unmask (bit 16 = 0)
  // Destination field (bits 56..63)
  redirect_entry |= ((uint64_t)dest_apic_id << 56);

  ioapic_set_entry(irq_line, redirect_entry);
}

void apic_disable_irq(uint8_t irq_line) {
  // To disable, we set the mask bit (bit 16)
  uint64_t redirect_entry = (1 << 16);
  ioapic_set_entry(irq_line, redirect_entry);
}

void apic_mask_all(void) {
  // Mask all 24 redirection entries in the I/O APIC
  for (int i = 0; i < 24; i++) {
    apic_disable_irq(i);
  }
}

// --- APIC Timer Management ---

void apic_timer_init(uint32_t frequency_hz) {
  // Calibrate and set the initial count
  apic_timer_set_frequency(frequency_hz);
  printk(APIC_CLASS "Timer installed at %d Hz.\n", frequency_hz);
}

void apic_timer_set_frequency(uint32_t frequency_hz) {
  if (frequency_hz == 0)
    return;

  g_apic_timer_hz = frequency_hz;

  // Use PIT to calibrate
  // 1. Prepare LAPIC Timer: Divide by 16, One-shot, Masked
  lapic_write(LAPIC_TIMER_DIV, 0x3);       // Divide by 16
  lapic_write(LAPIC_LVT_TIMER, (1 << 16)); // Masked

  // 2. Prepare PIT: Channel 2, Mode 0, Rate = 11931 (approx 10ms)
  // 1193182 Hz / 11931 ~= 100 Hz (10ms)
  uint16_t pit_reload = 11931;
  outb(0x61, (inb(0x61) & 0xFD) | 1); // Gate high
  outb(0x43, 0xB0);                   // Channel 2, Access Lo/Hi, Mode 0, Binary
  outb(0x42, pit_reload & 0xFF);
  outb(0x42, (pit_reload >> 8) & 0xFF);

  // 3. Reset APIC Timer to -1
  lapic_write(LAPIC_TIMER_INIT_COUNT, 0xFFFFFFFF);

  // 4. Wait for PIT to wrap (10ms)
  while (!(inb(0x61) & 0x20))
    ;

  // 5. Stop APIC timer
  lapic_write(LAPIC_LVT_TIMER, (1 << 16));

  // 6. Calculate ticks
  uint32_t ticks_in_10ms = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR_COUNT);

  // 7. Calculate ticks per period for target frequency
  // ticks_in_10ms corresponds to 100Hz (0.01s)
  // ticks_per_sec = ticks_in_10ms * 100
  // target = ticks_per_sec / frequency_hz
  uint32_t ticks_per_target = (ticks_in_10ms * 100) / frequency_hz;

  // 8. Start Timer: Periodic, Interrupt Vector 32, Unmasked
  // Bit 17 = 1 for Periodic mode, Bit 16 = 0 for Unmasked
  uint32_t lvt_timer = 32 | (1 << 17); // Vector 32, Periodic mode, Unmasked
  lapic_write(LAPIC_LVT_TIMER, lvt_timer);
  lapic_write(LAPIC_TIMER_DIV, 0x3); // Divide by 16
  lapic_write(LAPIC_TIMER_INIT_COUNT, ticks_per_target);

  printk(APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u\n", lvt_timer,
         ticks_per_target);
}

// --- Private Setup Functions ---

// Check for APIC presence via CPUID
static bool detect_apic(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, &eax, &ebx, &ecx, &edx);
  return (edx & (1 << 9)) != 0; // Check for APIC feature bit
}

// Initialize the Local APIC
bool setup_lapic(void) {
  // Get LAPIC physical base address from MSR
  uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
  uint64_t lapic_phys_base = lapic_base_msr & 0xFFFFFFFFFFFFF000ULL;

  printk(APIC_CLASS "LAPIC Physical Base: 0x%llx\n", lapic_phys_base);

  // Map the LAPIC into virtual memory using the VMM MMIO allocator
  // It will return a higher-half address (e.g. 0xFFFF9000...)
  g_lapic_base = (volatile uint32_t *)vmm_map_mmio(lapic_phys_base, PAGE_SIZE);

  if (!g_lapic_base) {
    printk(KERN_ERR APIC_CLASS "Failed to map LAPIC MMIO.\n");
    return false;
  }

  printk(APIC_CLASS "LAPIC Mapped at: 0x%llx\n", (uint64_t)g_lapic_base);

  // Enable the LAPIC by setting the enable bit in the MSR and the spurious
  // vector register
  wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE);

  // Set Spurious Interrupt Vector (0xFF) and enable APIC (bit 8)
  lapic_write(LAPIC_SVR, 0x1FF);

  // Set TPR to 0 to accept all interrupts
  lapic_write(LAPIC_TPR, 0);
  return true;
}

// Initialize the I/O APIC
bool setup_ioapic(void) {
  // Map the I/O APIC into virtual memory.
  // Note: In a real ACPI system, we should parse the MADT to find the actual
  // I/O APIC address. For now we assume the standard default.
  g_ioapic_base =
      (volatile uint32_t *)vmm_map_mmio(IOAPIC_DEFAULT_PHYS_ADDR, PAGE_SIZE);

  if (!g_ioapic_base) {
    printk(KERN_ERR APIC_CLASS "Failed to map I/O APIC MMIO.\n");
    return false;
  }

  printk(APIC_CLASS "IOAPIC Mapped at: 0x%llx\n", (uint64_t)g_ioapic_base);

  // Read the I/O APIC version to verify it's working
  uint32_t version_reg = ioapic_read(IOAPIC_REG_VER);
  printk(APIC_CLASS "IOAPIC Version: 0x%x\n", version_reg);

  // Mask all interrupts initially
  apic_mask_all();

  return true;
}