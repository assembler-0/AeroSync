#pragma once

/**
 * @file classes.h
 * @brief Kernel class prefixes for logging subsystems
 */

/* =========================================================================
 *  SYSTEM & ARCHITECTURE (Low Level)
 * ========================================================================= */
///@section Boot & Initialization
#define BOOT_CLASS "[aerosync::boot] " // Bootloader info, multiboot parsing
#define ACPI_CLASS "[aerosync::acpi] " // ACPI table parsing (RSDP, MADT, FADT)
#define ACPI_BUTTON_CLASS                                                      \
  "[aerosync::acpi::btn] " // ACPI Power/Sleep button handling
#define POWER_CLASS "[aerosync::acpi::power] "
#define HAL_CLASS                                                              \
  "[aerosync::sysintf::udm] " // Hardware Abstraction Layer generic (UDM)

///@section CPU & Interrupts
#define GDT_CLASS "[aerosync::gdt] "        // Global Descriptor Table
#define IDT_CLASS "[aerosync::irq::idt] "   // Interrupt Descriptor Table
#define ISR_CLASS "[aerosync::irq::isr] "   // Interrupt Service Routines
#define IRQ_CLASS "[aerosync::irq] "        // Hardware Interrupt Requests
#define APIC_CLASS "[aerosync::ic::apic] "  // IOAPIC / LAPIC configuration
#define PIC_CLASS "[aerosync::ic::pic] "    // Legacy PIC configuration
#define PIT_CLASS "[aerosync::timer::pit] " // Programmable Interval Timer
#define IC_CLASS                                                               \
  "[aerosync::sysintf::ic] " // Interrupt Controller (APIC/PIC switching)
#define SMP_CLASS                                                              \
  "[aerosync::cpu::smp] " // Symmetric Multi-Processing (Multicore startup)
#define TSC_CLASS "[aerosync::timer::tsc] "   // Time Stamp Counter / CPU timing
#define CPU_CLASS "[aerosync::cpu] "          // CPU features, MSRs, CPUID
#define FPU_CLASS "[aerosync::cpu::fpu] "     // Floating Point / SSE / AVX contexts
#define HPET_CLASS "[aerosync::timer::hpet] " // High Precision Event Timer
#define TIME_CLASS "[aerosync::sysintf::time] " // Unified timer subsystem
#define PERCPU_CLASS "[aerosync::cpu::percpu] " // Per-CPU data

///@section Core Kernel
#define KERN_CLASS "[aerosync::core] "         // Generic kernel core messages
#define PANIC_CLASS "[aerosync::core::panic] " // Kernel panics (Fatal errors)
#define FAULT_CLASS "[aerosync::core::panic::fault] "
#define SYSCALL_CLASS "[aerosync::core::syscall] " // System call entry/exit tracing
#define ATOMIC_CLASS "[aerosync::core::atomic] "   // Atomic operations
#define FW_CLASS "[aerosync::core::fw] "           // Firmware interfaces (BIOS/UEFI)
#define NVRAM_CLASS "[aerosync::core::fw::nvram] "
#define SMBIOS_CLASS "[aerosync::core::fw::smbios] " // SMBIOS parsing
#define FKX_CLASS "[aerosync::sysintf::fkx] "        // FKX Module Loader
#define ASRX_CLASS "[aerosync::sysintf::asrx] "
#define SYNC_CLASS                                                             \
  "[aerosync::core::sync] " // Synchronization (Mutex, Semaphores, Spinlocks)
#define NUMA_CLASS "[aerosync::core::numa] "
#define STACKTRACE_CLASS "[aerosync::core::stacktrace] "
#define LMM_CLASS "[aerosync::core::lmm] "

///@section Crypto
#define RNG_CLASS "[aerosync::crypto::rng] " // Random Number Generator
#define CRC_CLASS "[aerosync::crypto::crc] " // CRC32
#define SHA_CLASS "[aerosync::crypto::sha] " // SHA*
#define AES_CLASS "[aerosync::crypto::aes] "
#define RSA_CLASS "[aerosync::crypto::rsa] "
#define CRYPTO_CLASS "[aerosync::crypto::core] "

/* =========================================================================
 *  SANITIZER
 * ========================================================================= */
#define UBSAN_CLASS "[aerosync::san::ubsan] " // Undefined Behavior Sanitizer
#define ASAN_CLASS "[aerosync::san:asan] "    // Address Sanitizer
#define TSAN_CLASS "[aerosync::san::tsan] "   // Thread Sanitizer
#define MSAN_CLASS "[aerosync::san::msan] "   // Memory Sanitizer
#define LSAN_CLASS "[aerosync::san::lsan] "   // Leak Sanitizer
#define CFI_CLASS "[aerosync::san::cfi] "     // Control Flow Integrity

/* =========================================================================
 *  MEMORY MANAGEMENT
 * ========================================================================= */
///@section Physical & Virtual Memory
#define PMM_CLASS "[aerosync::mm::pm] " // Physical Memory Manager (Bitmap/Buddy)
#define VMM_CLASS "[aerosync::mm::vm] " // Virtual Memory
#define SWAP_CLASS "[aerosync::mm::vm::swap] " // Swap space / Paging to disk
#define MMIO_CLASS "[aerosync::mm::vm::mmio] " // MMIO Virtual Address Allocator
#define IOMMU_CLASS "[aerosync::mm::vm::iommu] "
#define VMA_CLASS "[aerosync::mm::vm::vma] " // Virtual Memory Area
#define FOLIO_CLASS "[aerosync::mm::folio] " // Linux struct folio
#define WRITEBACK_CLASS "[aerosync::mm::vm::writeback] "
#define THP_CLASS "[aerosync::mm::vm::thp] "
#define DMA_CLASS "[aerosync::mm::pm::dma] "
#define KSM_CLASS "[aerosync::mm::ksm] "
#define UFFD_CLASS "[aerosync::mm::uffd] "

///@section Heaps & Allocators
#define SLAB_CLASS "[aerosync::mm::slab] " // Slab allocator specific
#define SHM_CLASS "[aerosync::mm::shm] "   // Shared Memory (IPC)

///@section Stack protectionv
#define STACK_CLASS "[aerosync::mm::stack] " // Stack overflow protection

/* =========================================================================
 *  PROCESS MANAGEMENT & SCHEDULING
 * ========================================================================= */
#define SCHED_CLASS                                                            \
  "[aerosync::sched] " // Scheduler (Context switching, Picking tasks)
#define RT_CLASS "[aerosync::sched::rt] "
#define DL_CLASS "[aerosync::sched::dl] "
#define TASK_CLASS "[aerosync::sched::task] " // Task creation/destruction logic
#define ELF_CLASS "[aerosync::sched::elf] "   // ELF Loader / Binary parser
#define IPC_CLASS                                                              \
  "[aerosync::sched::ipc] " // Inter-Process Communication (Pipes, MsgQueues)
#define SIGNAL_CLASS "[aerosync::sched::signal] " // POSIX Signals delivery

/* =========================================================================
 *  DEVICE DRIVERS
 * ========================================================================= */
///@section Bus Drivers
#define PCI_CLASS                                                              \
  "[aerosync::sub::pci] " /// @note PCI IS NOT A DRIVER! ITS MORE OF A SUBSYSTEM!
#define USB_CLASS "[aerosync::driver::usb] " // USB Stack (UHCI/EHCI/XHCI)

///@section Storage Drivers
#define BLOCK_CLASS "[aerosync::driver::storage] "
#define CHAR_CLASS "[aerosync::driver::char] "
#define ATA_CLASS "[aerosync::driver::storage::ata] "         // IDE/PATA support
#define AHCI_CLASS "[aerosync::driver::storage::ahci] "       // SATA support
#define NVME_CLASS "[aerosync::driver::storage::nvme] "       // NVMe SSD support
#define RAMDISK_CLASS "[aerosync::driver::storage::ramdisk] " // Initrd / Ramdisk
#define VIRTIO_BLK_CLASS "[aerosync::driver::storage::virtio] "

///@section Human Interface Devices
#define KBD_CLASS "[aerosync::driver::hid::keyboard] " // PS/2 or USB Keyboard
#define MOUSE_CLASS "[aerosync::driver::hid::mouse] "  // PS/2 or USB Mouse
#define HID_CLASS "[aerosync::driver::hid] "           // Generic HID

///@section Display & Graphics
#define VIDEO_CLASS "[aerosync::driver::video] "    // VGA / VESA / GOP / Framebuffer
#define GPU_CLASS "[aerosync::driver::video::gpu] " // Hardware Acceleration
#define TTY_CLASS "[aerosync::driver::video::tty] " // Terminal / Console output
#define PTY_CLASS "[aerosync::driver::video::pty] " // Terminal / Console output

///@section Audio & Misc
#define AUDIO_CLASS "[aerosync::driver::audio] "       // AC97 / Intel HDA
#define SERIAL_CLASS "[aerosync::driver::misc::uart] " // UART / Serial Port

/* =========================================================================
 *  FILESYSTEMS (VFS)
 * ========================================================================= */
#define VFS_CLASS "[aerosync::fs::vfs] "   // Virtual File System (Mounts, nodes)
#define FAT_CLASS "[aerosync::fs::fat] "   // FAT12/16/32 Driver
#define EXT_CLASS "[aerosync::fs::ext] "   // EXT2/3/4 Driver
#define ISO_CLASS "[aerosync::fs::iso] "   // ISO9660 (CD-ROM)
#define DEVTMPFS_CLASS "[aerosync::fs::devtmpfs] " // /dev filesystem
#define TMPFS_CLASS "[aerosync::fs::tmp] "
#define SYSFS_CLASS "[aerosync::fs::sysfs] "
#define NTFS_CLASS "[aerosync::fs::ntfs] "
#define USTAR_CLASS "[aerosync::fs::ustar] "
#define INITRD_CLASS "[aerosync::fs::initrd] "
#define NEWC_CLASS "[aerosync::fs::newc] "
#define CPIO_CLASS "[aerosync::fs::cpio] "
#define PROCFS_CLASS "[aerosync::fs::proc] "
#define RESFS_CLASS "[aerosync::fs::res] "

/* =========================================================================
 *  NETWORKING STACK
 * ========================================================================= */
#define NET_CLASS "[aerosync::net] " // Generic Network Stack
#define NIC_CLASS                                                              \
  "[aerosync::net::driver::nic] " // Network Interface Card Driver (e1000, rtl8139)
#define ETH_CLASS "[aerosync::net::eth] "   // Ethernet Layer (L2)
#define IP_CLASS "[aerosync::net::ip] "     // IPv4/IPv6 Layer (L3)
#define ARP_CLASS "[aerosync::net::arp] "   // ARP Protocol
#define TCP_CLASS "[aerosync::net::tcp] "   // TCP Protocol (L4)
#define UDP_CLASS "[aerosync::net::udp] "   // UDP Protocol (L4)
#define DHCP_CLASS "[aerosync::net::dhcp] " // DHCP Client

/* =========================================================================
 *  UTILITIES & LIBRARIES
 * ========================================================================= */
#define TEST_CLASS "[aerosync::misc::test] " // Unit tests running inside kernel
#define DEBUG_CLASS "[aerosync::misc::debug] "
#define iKDB_CLASS "[aerosync::core::ikdb] "