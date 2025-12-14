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
    // In Mode 0, it counts down to 0.
    // We simply poll the count.

    while (1) {
      // Latch Counter 0
      outb(PIT_CMD_PORT, 0x00);
      uint8_t lo = inb(PIT_CH0_PORT);
      uint8_t hi = inb(PIT_CH0_PORT);
      uint16_t val = ((uint16_t)hi << 8) | lo;

      // Should be decreasing.
      // When it hits 0 undefined? In Mode 0 it wraps to FFFF or 0?
      // 8254: "Output goes high on terminal count".
      // Let's assume if val is large (it wrapped) or very small, we depend on
      // time passing. Robust check: if val > count (it wrapped?) -> done?
      // Actually, just checking if it is small is flaky.
      // Better: use Mode 0, wait for OUT bit. If that failed, maybe I
      // implemented it wrong. Let's TRY just looping until count is small, e.g.
      // < 1000 ticks. But we might miss it.

      // Revert back to Status Byte with correct logic.
      // 0xE2 is Read-Back Status for Ch0.
      outb(PIT_CMD_PORT, 0xE2);
      uint8_t status = inb(PIT_CH0_PORT);
      if (status & 0x80) { // OUT set
        break;
      }

      // Failsafe: if we read status as 0xFF (bus float?), we might exit early.
      // If we loop too long?
    }
  }

  // Restore generic frequency (100Hz default) or whatever it was?
  // We should probably restore global_pit_frequency.
  pit_set_frequency(global_pit_frequency);

  restore_irq_flags(flags);
}
