#pragma once

/**
 * @file classes.h
 * @brief Kernel class prefixes for logging subsystems
 */

/* =========================================================================
 *  SYSTEM & ARCHITECTURE (Low Level)
 * ========================================================================= */
///@section Boot & Initialization
#define BOOT_CLASS "[sys::boot] " // Bootloader info, multiboot parsing
#define ACPI_CLASS "[sys::acpi] " // ACPI table parsing (RSDP, MADT, FADT)
#define ACPI_BUTTON_CLASS "[sys::acpi::btn] " // ACPI Power/Sleep button handling
#define POWER_CLASS "[sys::acpi::power] "
#define HAL_CLASS "[sys::sysintf::udm] "   // Hardware Abstraction Layer generic (UDM)

///@section CPU & Interrupts
#define GDT_CLASS "[sys::gdt] "   // Global Descriptor Table
#define IDT_CLASS "[sys::irq::idt] "   // Interrupt Descriptor Table
#define ISR_CLASS "[sys::irq::isr] "   // Interrupt Service Routines
#define IRQ_CLASS "[sys::irq] "   // Hardware Interrupt Requests
#define APIC_CLASS "[sys::ic::apic] " // IOAPIC / LAPIC configuration
#define PIC_CLASS "[sys::ic::PIC] "   // Legacy PIC configuration
#define PIT_CLASS "[sys::timer::PIT] "   // Programmable Interval Timer
#define IC_CLASS "[sys::sysintf::ic] "     // Interrupt Controller (APIC/PIC switching)
#define SMP_CLASS "[sys::cpu::smp] " // Symmetric Multi-Processing (Multicore startup)
#define TSC_CLASS "[sys::timer::tsc] " // Time Stamp Counter / CPU timing
#define CPU_CLASS "[sys::cpu] " // CPU features, MSRs, CPUID
#define FPU_CLASS "[sys::cpu::fpu] " // Floating Point / SSE / AVX contexts
#define HPET_CLASS "[sys::timer::hpet] " // High Precision Event Timer
#define TIME_CLASS "[sys::sysintf::time] " // Unified timer subsystem
#define PERCPU_CLASS "[sys::cpu::percpu] " // Per-CPU data

///@section Core Kernel
#define KERN_CLASS "[sys::core] "         // Generic kernel core messages
#define PANIC_CLASS "[sys::core::panic] "       // Kernel panics (Fatal errors)
#define FAULT_CLASS "[sys::core::panic::fault] "
#define SYSCALL_CLASS "[sys::core::syscall] "      // System call entry/exit tracing
#define ATOMIC_CLASS "[sys::core::atomic] "     // Atomic operations
#define FW_CLASS "[sys::core::fw] "           // Firmware interfaces (BIOS/UEFI)
#define SMBIOS_CLASS "[sys::core::fw::smbios] " // SMBIOS parsing
#define FKX_CLASS "[sys::sysintf::fkx] "         // FKX Module Loader
#define SYNC_CLASS "[sys::core::sync] " // Synchronization (Mutex, Semaphores, Spinlocks)
#define NUMA_CLASS "[sys::core::numa] "
#define STACKTRACE_CLASS "[sys::core::stacktrace] "
#define LMM_CLASS "[sys::core::lmm] "

///@section Crypto
#define RNG_CLASS "[crypto::rng] " // Random Number Generator
#define CRC_CLASS "[crypto::crc] " // CRC32
#define SHA_CLASS "[crypto::sha] " // SHA*

/* =========================================================================
 *  SANITIZER
 * ========================================================================= */
#define UBSAN_CLASS "[sys::san::ubsan] " // Undefined Behavior Sanitizer
#define ASAN_CLASS "[sys::san:asan] "   // Address Sanitizer
#define TSAN_CLASS "[sys::san::tsan] "   // Thread Sanitizer
#define MSAN_CLASS "[sys::san::msan] "   // Memory Sanitizer
#define LSAN_CLASS "[sys::san::lsan] "   // Leak Sanitizer

/* =========================================================================
 *  MEMORY MANAGEMENT
 * ========================================================================= */
///@section Physical & Virtual Memory
#define PMM_CLASS "[sys::mm::pmm] "   // Physical Memory Manager (Bitmap/Buddy)
#define VMM_CLASS "[sys::mm::vmm] "   // Virtual Memory Manager (Paging, PDE/PTE)
#define SWAP_CLASS "[sys::mm::swap] " // Swap space / Paging to disk
#define MMIO_CLASS "[sys::mm::mmio] " // MMIO Virtual Address Allocator
#define VMA_CLASS "[sys::mm::vma] " // Virtual Memory Area
#define FOLIO_CLASS "[sys::mm::folio] " // Linux struct folio
#define WRITEBACK_CLASS "[sys::mm::writeback] "
#define THP_CLASS "[sys::mm::thp] "

///@section Heaps & Allocators
#define SLAB_CLASS "[sys::mm::slab] " // Slab allocator specific
#define SHM_CLASS "[sys::mm::shm] "   // Shared Memory (IPC)

///@section Stack protectionv
#define STACK_CLASS "[sys::mm::stack] " // Stack overflow protection

/* =========================================================================
 *  PROCESS MANAGEMENT & SCHEDULING
 * ========================================================================= */
#define SCHED_CLASS "[sys::sched] " // Scheduler (Context switching, Picking tasks)
#define TASK_CLASS "[sys::sched::task] "   // Task creation/destruction logic
#define ELF_CLASS "[sys::sched::elf] "     // ELF Loader / Binary parser
#define IPC_CLASS "[sys::sched::ipc] " // Inter-Process Communication (Pipes, MsgQueues)
#define SIGNAL_CLASS "[sys::sched::signal] " // POSIX Signals delivery

/* =========================================================================
 *  DEVICE DRIVERS
 * ========================================================================= */
///@section Bus Drivers
#define PCI_CLASS "[sys::sub::pci] " /// @note PCI IS NOT A DRIVER! ITS MORE OF A SUBSYSTEM!
#define USB_CLASS "[sys::driver::usb] " // USB Stack (UHCI/EHCI/XHCI)

///@section Storage Drivers
#define BLOCK_CLASS "[sys::driver::storage] "
#define CHAR_CLASS "[sys::driver::char] "
#define ATA_CLASS "[sys::driver::storage::ata] "      // IDE/PATA support
#define AHCI_CLASS "[sys::driver::storage::ahci] "    // SATA support
#define NVME_CLASS "[sys::driver::storage::nvme] "    // NVMe SSD support
#define RAMDISK_CLASS "[sys::driver::storage::ramdisk] " // Initrd / Ramdisk
#define VIRTIO_BLK_CLASS "[sys::driver::storage::virtio] "

///@section Human Interface Devices
#define KBD_CLASS "[sys::driver::hid::keyboard] "     // PS/2 or USB Keyboard
#define MOUSE_CLASS "[sys::driver::hid::mouse] " // PS/2 or USB Mouse
#define HID_CLASS "[sys::driver::hid] "     // Generic HID

///@section Display & Graphics
#define VIDEO_CLASS "[sys::driver::video] " // VGA / VESA / GOP / Framebuffer
#define GPU_CLASS "[sys::driver::video::gpu] "   // Hardware Acceleration
#define TTY_CLASS "[sys::driver::video::tty] "   // Terminal / Console output
#define PTY_CLASS "[sys::driver::video::pty] "   // Terminal / Console output

///@section Audio & Misc
#define AUDIO_CLASS "[sys::driver::audio] "  // AC97 / Intel HDA
#define SERIAL_CLASS "[sys::driver::misc::uart] " // UART / Serial Port

/* =========================================================================
 *  FILESYSTEMS (VFS)
 * ========================================================================= */
#define VFS_CLASS "[sys::fs::vfs] "   // Virtual File System (Mounts, nodes)
#define FAT_CLASS "[sys::fs::fat] "   // FAT12/16/32 Driver
#define EXT_CLASS "[sys::fs::ext] "   // EXT2/3/4 Driver
#define ISO_CLASS "[sys::fs::iso] "   // ISO9660 (CD-ROM)
#define DEVFS_CLASS "[sys::fs::dev] " // /dev filesystem
#define TMPFS_CLASS "[sys::fs::tmp] "
#define NTFS_CLASS "[sys::fs::ntfs] "
#define USTAR_CLASS "[sys::fs::ustar] "
#define INITRD_CLASS "[sys::fs::initrd] "
#define NEWC_CLASS "[sys::fs::newc] "
#define CPIO_CLASS "[sys::fs::cpio] "

/* =========================================================================
 *  NETWORKING STACK
 * ========================================================================= */
#define NET_CLASS "[sys::net] " // Generic Network Stack
#define NIC_CLASS "[sys::net::driver::nic] " // Network Interface Card Driver (e1000, rtl8139)
#define ETH_CLASS "[sys::net::eth] "   // Ethernet Layer (L2)
#define IP_CLASS "[sys::net::ip] "   // IPv4/IPv6 Layer (L3)
#define ARP_CLASS "[sys::net::arp] "   // ARP Protocol
#define TCP_CLASS "[sys::net::tcp] "   // TCP Protocol (L4)
#define UDP_CLASS "[sys::net::udp] "   // UDP Protocol (L4)
#define DHCP_CLASS "[sys::net::dhcp] " // DHCP Client

/* =========================================================================
 *  UTILITIES & LIBRARIES
 * ========================================================================= */
#define TEST_CLASS "[misc::test] " // Unit tests running inside kernel
#define DEBUG_CLASS "[misc::debug] "