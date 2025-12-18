#include <arch/x64/cpu.h>
#include <arch/x64/io.h>
#include <drivers/timer/pit.h>
#include <lib/printk.h>

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CH1_PORT 0x41
#define PIT_CH2_PORT 0x42

static uint32_t global_pit_frequency = 100; // Default

void pit_set_frequency(uint32_t frequency) {
  if (frequency == 0)
    frequency = 100; // Safe default
  if (frequency > PIT_FREQUENCY_BASE)
    frequency = PIT_FREQUENCY_BASE;

  global_pit_frequency = frequency;

  uint32_t divisor = PIT_FREQUENCY_BASE / frequency;
  if (divisor > 65535)
    divisor = 65535;

  // Save IRQ state
  irq_flags_t flags = save_irq_flags();

  // Mode 2 (Rate Generator), Binary, Channel 0
  // 00 11 010 0 -> 0x34
  // 0x36 = 00 11 011 0 (Channel 0, LOHI, Mode 3 (Square Wave), Binary)
  // Linux often uses Mode 2 or 3. Mode 3 is square wave (good for sound/timer).
  outb(PIT_CMD_PORT, 0x36);
  outb(PIT_CH0_PORT, divisor & 0xFF);
  outb(PIT_CH0_PORT, (divisor >> 8) & 0xFF);

  restore_irq_flags(flags);
}

// Spin-wait using PIT Channel 0
// NOTE: This modifies Channel 0 configuration temporarily!
// It should only be used during early boot for calibration.
// If used later, it will disrupt the system timer interrupt frequency.
void pit_wait(uint32_t ms) {
  // We can't really "wait" using the standard timer interrupt mode easily
  // without interrupts enabled and an ISR counting. For calibration, we often
  // want to busy wait WITHOUT interrupts enabled.

  // Valid Range for PIT wait in Mode 0 (Interrupt on Terminal Count)
  // Max wait = 65535 / 1193182 ~= 54ms.
  // If ms > 50, we should loop.

  irq_flags_t flags = save_irq_flags();

  while (ms > 0) {
    uint32_t chunk_ms = (ms > 50) ? 50 : ms;
    ms -= chunk_ms;

    // Calculate count for this chunk
    uint16_t count = (uint16_t)((PIT_FREQUENCY_BASE * chunk_ms) / 1000);

    // Mode 0: Interrupt on Terminal Count
    // 00 11 000 0 -> 0x30 (Channel 0, LOHI, Mode 0, Binary)
    outb(PIT_CMD_PORT, 0x30);
    outb(PIT_CH0_PORT, count & 0xFF);
    outb(PIT_CH0_PORT, (count >> 8) & 0xFF);

    // Wait for count to hit 0.
    // In Mode 0, output goes high when count reaches 0.
    // We can check the status byte if 0xE2 (Read-Back) command is supported,
    // or just read the counter until it's very small/wraps?
    // Actually, in Mode 0, reading the counter works. It counts down.

    // Wait for count to hit 0.
    // In Mode 0, it counts down to 0 and then wraps to 0xFFFF (or stops depending on implementation).
    // We use the Latch Command (0x00) which is 8253 compatible and safer on emulators.
    
    while (1) {
      // Latch Counter 0
      outb(PIT_CMD_PORT, 0x00);
      uint8_t lo = inb(PIT_CH0_PORT);
      uint8_t hi = inb(PIT_CH0_PORT);
      uint16_t current_val = ((uint16_t)hi << 8) | lo;

      // Check if we reached 0 or wrapped around (current_val > count)
      // Note: We check current_val > count because count < 65535 (chunk_ms <= 50 -> count <= ~59659)
      // If it wraps to 0xFFFF, it will be > count.
      if (current_val == 0 || current_val > count) {
        break;
      }
      
      cpu_relax();
    }
  }

  // Restore generic frequency (100Hz default) or whatever it was?
  // We should probably restore global_pit_frequency.
  pit_set_frequency(global_pit_frequency);

  restore_irq_flags(flags);
}
