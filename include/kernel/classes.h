#pragma once

/**
 * @file classes.h
 * @brief Kernel class prefixes for logging subsystems
 */

/* =========================================================================
 *  SYSTEM & ARCHITECTURE (Low Level)
 * ========================================================================= */
///@section Boot & Initialization
#define BOOT_CLASS "[SYS::BOOT] " // Bootloader info, multiboot parsing
#define ACPI_CLASS "[SYS::ACPI] " // ACPI table parsing (RSDP, MADT, FADT)
#define HAL_CLASS "[SYS::HAL] "   // Hardware Abstraction Layer generic

///@section CPU & Interrupts
#define GDT_CLASS "[SYS::GDT] "   // Global Descriptor Table
#define IDT_CLASS "[SYS::IDT] "   // Interrupt Descriptor Table
#define ISR_CLASS "[SYS::ISR] "   // Interrupt Service Routines
#define IRQ_CLASS "[SYS::IRQ] "   // Hardware Interrupt Requests
#define APIC_CLASS "[SYS::APIC] " // IOAPIC / LAPIC configuration
#define PIC_CLASS "[SYS::PIC] "   // Legacy PIC configuration
#define PIT_CLASS "[SYS::PIT] "   // Programmable Interval Timer
#define IC_CLASS "[SYS::IC] "     // Interrupt Controller (APIC/PIC switching)
#define SMP_CLASS "[SYS::SMP] " // Symmetric Multi-Processing (Multicore startup)
#define TSC_CLASS "[SYS::TSC] " // Time Stamp Counter / CPU timing
#define CPU_CLASS "[SYS::CPU] " // CPU features, MSRs, CPUID
#define FPU_CLASS "[SYS::FPU] " // Floating Point / SSE / AVX contexts

///@section Core Kernel
#define KERN_CLASS "[SYS::CORE] "         // Generic kernel core messages
#define PANIC_CLASS "[SYS::PANIC] "       // Kernel panics (Fatal errors)
#define SYSCALL_CLASS "[SYS::CALL] "      // System call entry/exit tracing
#define TIME_CLASS "[SYS::TIME] "         // RTC, PIT, HPET, System Clock
#define ATOMIC_CLASS "[SYS::ATOMIC] "     // Atomic operations
#define SPINLOCK_CLASS "[SYS::SPINLOCK] " // Spinlock operations

///@section Crypto
#define RNG_CLASS "[CRPT::RNG] " // Random Number Generator
#define CRC_CLASS "[CRPT::CRC] " // CRC32
#define SHA_CLASS "[CRPT::SHA] " // SHA*

/* =========================================================================
 *  SANITIZER
 * ========================================================================= */
#define UBSAN_CLASS "[SAN::UBSAN] " // Undefined Behavior Sanitizer
#define ASAN_CLASS "[SAN::ASAN] "   // Address Sanitizer
#define TSAN_CLASS "[SAN::TSAN] "   // Thread Sanitizer
#define MSAN_CLASS "[SAN::MSAN] "   // Memory Sanitizer
#define LSAN_CLASS "[SAN::LSAN] "   // Leak Sanitizer

/* =========================================================================
 *  MEMORY MANAGEMENT
 * ========================================================================= */
///@section Physical & Virtual Memory
#define PMM_CLASS "[MM::PMM] "   // Physical Memory Manager (Bitmap/Buddy)
#define VMM_CLASS "[MM::VMM] "   // Virtual Memory Manager (Paging, PDE/PTE)
#define PAGE_CLASS "[MM::PAGE] " // Page Fault Handler
#define SWAP_CLASS "[MM::SWAP] " // Swap space / Paging to disk
#define MMIO_CLASS "[MM::MMIO] " // MMIO Virtual Address Allocator
#define VMA_CLASS "[MM::VMA] " // Virtual Memory Area

///@section Heaps & Allocators
#define SLAB_CLASS "[MM::SLAB] " // Slab allocator specific
#define SHM_CLASS "[MM::SHM] "   // Shared Memory (IPC)

///@section Stack protection
#define STACK_CLASS "[MM::STACK] " // Stack overflow protection

/* =========================================================================
 *  PROCESS MANAGEMENT & SCHEDULING
 * ========================================================================= */
#define SCHED_CLASS                                                            \
  "[PROC::SCHED] " // Scheduler (Context switching, Picking tasks)
#define TASK_CLASS "[PROC::TASK] "   // Task creation/destruction logic
#define THREAD_CLASS "[PROC::THRD] " // Thread specific logic
#define ELF_CLASS "[PROC::ELF] "     // ELF Loader / Binary parser
#define IPC_CLASS                                                              \
  "[PROC::IPC] " // Inter-Process Communication (Pipes, MsgQueues)
#define SYNC_CLASS                                                             \
  "[PROC::SYNC] " // Synchronization (Mutex, Semaphores, Spinlocks)
#define SIGNAL_CLASS "[PROC::SIG] " // POSIX Signals delivery

/* =========================================================================
 *  DEVICE DRIVERS
 * ========================================================================= */
///@section Bus Drivers
#define PCI_CLASS "[DRV::PCI] " // PCI/PCIe Bus enumeration
#define USB_CLASS "[DRV::USB] " // USB Stack (UHCI/EHCI/XHCI)

///@section Storage Drivers
#define ATA_CLASS "[DRV::ATA] "      // IDE/PATA support
#define AHCI_CLASS "[DRV::AHCI] "    // SATA support
#define NVME_CLASS "[DRV::NVME] "    // NVMe SSD support
#define RAMDISK_CLASS "[DRV::RAMD] " // Initrd / Ramdisk

///@section Human Interface Devices
#define KBD_CLASS "[DRV::KBD] "     // PS/2 or USB Keyboard
#define MOUSE_CLASS "[DRV::MOUSE] " // PS/2 or USB Mouse
#define HID_CLASS "[DRV::HID] "     // Generic HID

///@section Display & Graphics
#define VIDEO_CLASS "[DRV::VID] " // VGA / VESA / GOP / Framebuffer
#define GPU_CLASS "[DRV::GPU] "   // Hardware Acceleration
#define TTY_CLASS "[DRV::TTY] "   // Terminal / Console output

///@section Audio & Misc
#define AUDIO_CLASS "[DRV::AUD] "  // AC97 / Intel HDA
#define SERIAL_CLASS "[DRV::COM] " // UART / Serial Port

/* =========================================================================
 *  FILESYSTEMS (VFS)
 * ========================================================================= */
#define VFS_CLASS "[FS::VFS] "   // Virtual File System (Mounts, nodes)
#define FAT_CLASS "[FS::FAT] "   // FAT12/16/32 Driver
#define EXT_CLASS "[FS::EXT] "   // EXT2/3/4 Driver
#define ISO_CLASS "[FS::ISO] "   // ISO9660 (CD-ROM)
#define DEVFS_CLASS "[FS::DEV] " // /dev filesystem

/* =========================================================================
 *  NETWORKING STACK
 * ========================================================================= */
#define NET_CLASS "[NET::CORE] " // Generic Network Stack
#define NIC_CLASS                                                              \
  "[NET::NIC] " // Network Interface Card Driver (e1000, rtl8139)
#define ETH_CLASS "[NET::ETH] "   // Ethernet Layer (L2)
#define IP_CLASS "[NET::IPV4] "   // IPv4/IPv6 Layer (L3)
#define ARP_CLASS "[NET::ARP] "   // ARP Protocol
#define TCP_CLASS "[NET::TCP] "   // TCP Protocol (L4)
#define UDP_CLASS "[NET::UDP] "   // UDP Protocol (L4)
#define DHCP_CLASS "[NET::DHCP] " // DHCP Client

/* =========================================================================
 *  UTILITIES & LIBRARIES
 * ========================================================================= */
#define TEST_CLASS "[SYS::TEST] " // Unit tests running inside kernel