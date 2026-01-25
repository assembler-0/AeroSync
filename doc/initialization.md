# AeroSync Initialization Documentation

## Overview

The AeroSync kernel initialization process begins in `start_kernel()` and proceeds through several distinct phases to bring up the system. The initialization sequence is carefully orchestrated to ensure that each subsystem is ready before dependent components are initialized.

## Entry Point

The kernel entry point is `start_kernel()` in `init/main.c`, which is called by the bootloader after basic hardware setup. This function never returns and transitions the system from bootloader control to full kernel operation.

## Initialization Phases

### 1. Early Panic Handler Setup

```c
panic_register_handler(get_builtin_panic_ops());
panic_handler_install();
```

Sets up the panic handling infrastructure early in the boot process to ensure reliable crash reporting even during early initialization.

### 2. Bootloader Verification

```c
if (LIMINE_BASE_REVISION_SUPPORTED(get_limine_base_revision()) == false) {
    panic_early();
}
```

Verifies that the bootloader (Limine) meets the minimum revision requirements for proper kernel operation.

### 3. Early Console and Timing Initialization

```c
printk_register_backend(debugcon_get_backend());
printk_init_early();
tsc_calibrate_early();
```

- Registers the debug console as the early printk backend
- Initializes early printing capabilities
- Performs initial calibration of the Time Stamp Counter (TSC)

### 4. System Information Display

The kernel prints version information and bootloader details:

```c
printk(KERN_CLASS "AeroSync (R) %s - %s\n", AEROSYNC_VERSION,
       AEROSYNC_COMPILER_VERSION);
```

### 5. Firmware Detection

Detects and reports the firmware type (UEFI, BIOS, etc.):

```c
if (get_fw_request()->response) {
    printk(FW_CLASS "firmware type: %s\n",
       get_fw_request()->response->firmware_type == LIMINE_FIRMWARE_TYPE_EFI64
       ? "UEFI (64-bit)" : ...);
}
```

### 6. Command Line Parsing

The kernel parses command line options provided by the bootloader:

```c
cmdline_register_option("verbose", CMDLINE_TYPE_FLAG);
cmdline_parse(get_cmdline_request()->response->cmdline);
```

Supports options like `verbose` to enable debug logging.

### 7. Memory Management Initialization

This is a critical phase that initializes the memory management subsystem:

#### Physical Memory Manager (PMM)
```c
pmm_init(get_memmap_request()->response, get_hhdm_request()->response->offset,
         get_rsdp_request()->response ? get_rsdp_request()->response->address : NULL);
```

- Initializes the physical memory manager using the memory map provided by the bootloader
- Sets up the Higher Half Direct Memory mapping
- Manually parses the RSDP (XSDT) to locate SRAT and SLIT (uACPI is not active yet), this parsing is part of the kernel manual ACPI table parser

#### Virtual Memory Manager (VMM)
```c
vmm_init();
```

Initializes the virtual memory management system.

#### Memory Allocation Systems
```c
slab_init();           // Slab allocator for kernel objects
maple_tree_init();     // Maple tree data structure
vma_cache_init();      // Virtual memory area cache
radix_tree_init();     // Radix tree data structure
rcu_init();            // Read-Copy-Update mechanism
```

### 8. Per-CPU Data Structures

```c
setup_per_cpu_areas();
smp_prepare_boot_cpu();
pmm_init_cpu();
vmalloc_init();
```

- Sets up per-CPU memory areas for efficient multi-core operation
- Prepares the boot CPU for SMP operation
- Initializes CPU-specific PMM data
- Sets up the virtual memory allocator

### 9. Architecture-Specific Initialization

#### Global Descriptor Table (GDT)
```c
gdt_init();
```

Initializes the x86_64 Global Descriptor Table.

#### Interrupt Descriptor Table (IDT)
```c
idt_install();
```

Installs the Interrupt Descriptor Table for handling interrupts and exceptions.

#### System Calls
```c
syscall_init();
```

Initializes the system call interface.

### 10. Floating Point Unit (FPU) Initialization

```c
fpu_init();
```

Configures the FPU/SSE/AVX units for proper operation.

### 11. Scheduler Initialization

```c
sched_init();
bsp_task.active_mm = &init_mm;
sched_init_task(&bsp_task);
```

- Initializes the scheduler data structures
- Sets up the boot processor (BSP) task
- Associates the initial memory management context

### 12. Testing (Conditional)

If `INCLUDE_MM_TESTS` is defined and verbose mode is enabled (ik it sounds weird to have tests in verbose mode but whatever):

```c
#ifdef INCLUDE_MM_TESTS
if (cmdline_get_flag("verbose")) {
    pmm_test();
    vmm_test();
    slab_test();
    vma_test();
    vmalloc_test();
    vmalloc_dump();
}
#endif
```

Runs various memory management tests to verify correctness.

### 13. Module Loading

```c
system_load_extensions(get_module_request());
```

Loads FKX (Fused Kernel eXtension) modules provided by the bootloader.

### 14. Late Initialization Classes

The kernel registers various initialization classes for different subsystems:

```c
fkx_init_module_class(FKX_PRINTK_CLASS);
printk_init_late();

fkx_init_module_class(FKX_IC_CLASS);
ic_register_lapic_get_id_early();
```

### 15. Interrupt Controller Setup

```c
uacpi_kernel_init_early();
madt_init();
interrupt_controller_t ic_type = ic_install();
uacpi_notify_ic_ready();
```

- Initializes early uACPI support
- Parses the Multiple APIC Description Table (MADT), using uACPI
- Installs the appropriate interrupt controller (typically x2APIC)
- Notifies uACPI that the interrupt controller is ready

### 16. Time Subsystem Initialization

```c
// Initialize unified time subsystem (Selects Best Source and Inits it)
if (time_init() != 0) {
    printk(KERN_WARNING KERN_CLASS "Time subsystem initialization failed\n");
}

// Recalibrate TSC using the best available time source
if (tsc_freq_get() < 1000000) {
    if (time_calibrate_tsc_system() != 0) {
        printk(KERN_WARNING KERN_CLASS "TSC System Calibration failed.\n");
    } else {
        printk(KERN_CLASS "TSC calibrated successfully via %s.\n", time_get_source_name());
    }
} else {
    printk(KERN_CLASS "TSC already calibrated via CPUID (%lu Hz).\n", tsc_freq_get());
}

timer_init_subsystem();
```

- Initializes the unified time subsystem
- Calibrates the TSC using the best available timing source
- Initializes the timer subsystem

### 17. Advanced ACPI Initialization

```c
uacpi_kernel_init_late();
acpi_power_init();
```

Completes the uACPI initialization and sets up power management features.

### 18. Symmetric Multiprocessing (SMP) Initialization

```c
if (ic_type == INTC_APIC)
    smp_init();
```

Initializes SMP support if an APIC-based interrupt controller is present. (automatically deferred if SYMMETRIC_MP is not defined)

### 19. Additional Subsystems

```c
crc32_init();  // CRC32 checksum algorithm
vfs_init();    // Virtual File System
```

Initializes additional core subsystems.

### 20. Async Printk (Optional)

```c
#ifdef ASYNC_PRINTK
printk_init_async();
#endif
```

Enables asynchronous printk if the feature is compiled in.

### 21. Final Kernel Thread Creation

```c
struct task_struct *init_task = kthread_create(kernel_init, NULL, "kernel_init");
if (!init_task)
    panic(KERN_CLASS "Failed to create kernel_init thread");
kthread_run(init_task);

cpu_sti();  // Enable interrupts

// BSP becomes the idle thread
while (true) {
    check_preempt();
    cpu_hlt();
}
```

- Creates and starts the `kernel_init` kernel thread
- Enables interrupts on the boot processor
- The boot processor becomes the idle thread, running in a loop checking for work

## Secondary Initialization Thread

The `kernel_init()` function runs as a separate kernel thread and performs final system initialization:

```c
static int __init __noreturn __noinline __sysv_abi kernel_init(void *unused) {
    // Initialize memory management subsystems
    zmm_init();          // Zone management
    shm_init();          // Shared memory
    kswapd_init();       // Page swapping daemon
    khugepaged_init();   // Huge page daemon
    vm_writeback_init(); // Writeback mechanisms
    kvmap_purged_init(); // Purged VMAP management
    
    #ifdef MM_HARDENING
    mm_scrubber_init();  // Memory scrubbing for security
    #endif

    // TODO: Implement run_init_process() which calls do_execve()
    // For now, since we have no init binary on disk, we just stay in kernel
    printk(KERN_NOTICE KERN_CLASS "no init binary found. System idle.\n");

    printk(KERN_CLASS "AeroSync initialization complete.\n");

    while (1) {
        check_preempt();
        cpu_hlt();
    }
}
```

This thread initializes the remaining memory management components and then enters an idle loop.

> This initialization sequence ensures that all core kernel subsystems are properly initialized in the correct order, creating a stable foundation for system operation.