# AeroSync Kernel Initialization Documentation

> **Note:** This documentation describes the kernel initialization flow as implemented in `init/main.c`. All function calls and initialization order reflect the actual implementation. For configuration options that affect initialization, see the corresponding `Kconfig` entries.

## Overview

The AeroSync kernel initialization process begins at `start_kernel()` in `init/main.c`, which is called by the bootloader after basic hardware setup. The initialization follows a strict ordering to ensure subsystems are ready before dependent components are initialized. The boot CPU (BSP) performs all early initialization before releasing secondary CPUs via SMP initialization.

## Entry Point

```c
void __no_sanitize __init __noreturn __noinline __sysv_abi start_kernel(void)
```

**Location:** `init/main.c`

**Description:** Primary kernel entry point called after bootloader handoff. This function never returns and transitions the system from bootloader control to full kernel operation. The BSP becomes the idle thread after spawning the `kernel_init` thread.

**Constraints:**
- Called with interrupts disabled
- Executes in early boot context (no scheduler, no per-CPU areas)
- Must not sleep or block

---

## Initialization Phases

### Phase 1: Early Panic Handler Installation

```c
panic_register_handler(get_builtin_panic_ops());
panic_handler_install();
```

**Purpose:** Install the panic handling infrastructure before any operations that might fail. Ensures reliable crash reporting even during early initialization.

**Functions:**
- `panic_register_handler()` - Registers the built-in panic operations structure
- `panic_handler_install()` - Installs the panic handler as the active handler

---

### Phase 2: Bootloader Verification

```c
if (LIMINE_BASE_REVISION_SUPPORTED(get_limine_base_revision()) == false) {
    panic_early();
}
```

**Purpose:** Verify that the Limine bootloader meets the minimum revision requirements.

**Requirements:** Limine base revision must be supported. If not, the kernel triggers an early panic with minimal output.

---

### Phase 3: Early Console and Timing

```c
printk_register_backend(debugcon_get_backend());
aerosync_core_init_exprcall(printk_init_early(), printk_early);
tsc_calibrate_early();
```

**Purpose:** Establish early logging capability and calibrate the Time Stamp Counter (TSC).

**Functions:**
- `printk_register_backend()` - Registers the debugcon backend for early output
- `printk_init_early()` - Initializes early printk framework
- `tsc_calibrate_early()` - Performs initial TSC calibration using PIT/HPET

**Configuration:**
- Backend: QEMU debugcon port (0xe9)

---

### Phase 4: Command Line Parsing

```c
if (get_cmdline_request()->response) {
    if (cmdline_find_option_bool(current_cmdline, "quiet")) {
        printk_disable();
    }
    if (cmdline_find_option_bool(current_cmdline, "verbose")) {
        log_enable_debug();
    }
}
```

**Purpose:** Parse kernel command line options provided by the bootloader.

**Supported Options:**
- see [cmdline.md](cmdline.md)

**Configuration:**
- `CONFIG_COMMAND_LINE_SIZE` - Maximum command line buffer size (default: 4096)

---

### Phase 5: System Information Display

```c
printk(KERN_CLASS "AeroSync (R) %s - %s\n", AEROSYNC_VERSION,
       AEROSYNC_COMPILER_VERSION);
printk(KERN_CLASS "copyright (C) 2025-2026 assembler-0\n");
```

**Purpose:** Display kernel version and copyright information.

**Output:**
- Kernel version string from `aerosync/version.h`
- Compiler version string
- Copyright notice

---

### Phase 6: Kernel Symbol Table Initialization

```c
if (get_executable_file_request()->response &&
    get_executable_file_request()->response->executable_file) {
    aerosync_core_init(ksymtab_init, get_executable_file_request()->response->executable_file->address);
}
```

**Purpose:** Initialize the kernel symbol table for dynamic symbol resolution.

**Requirements:** Limine executable file response must be present.

---

### Phase 7: Bootloader and Firmware Information (Conditional)

```c
if (cmdline_find_option_bool(current_cmdline, "bootinfo")) {
    // Dump bootloader info, KASLR base, firmware type
}
```

**Purpose:** Display detailed bootloader and firmware information when `bootinfo` is specified.

**Output:**
- Bootloader name and version
- Execution and initialization timestamps
- KASLR virtual base address (if `kaslrinfo` specified)
- Firmware type (UEFI 64/32-bit, BIOS, SBI, or unknown)

---

### Phase 8: Memory Management Initialization

#### 8.1: CPU Features Detection

```c
aerosync_core_init(cpu_features_init);
```

Detects and records CPU feature flags (CPUID leaves).

#### 8.2: Physical Memory Manager (PMM)

```c
aerosync_core_init(pmm_init, get_memmap_request()->response, 
                   get_hhdm_request()->response->offset,
                   get_rsdp_request()->response
                     ? get_rsdp_request()->response->address
                     : nullptr);
```

**Purpose:** Initialize the physical memory manager.

**Parameters:**
- Memory map from Limine
- Higher Half Direct Map (HHDM) offset
- RSDP address (for manual ACPI table parsing: SRAT, SLIT)

**Configuration:** Uses bootloader-provided memory map.

#### 8.3: LRU List Initialization

```c
aerosync_core_init(lru_init);
```

Initializes LRU (Least Recently Used) page lists for page reclaim.

#### 8.4: Virtual Memory Manager (VMM)

```c
aerosync_core_init(vmm_init);
```

Initializes the virtual memory management system (page tables, address spaces).

#### 8.5: Slab Allocator

```c
aerosync_core_init(slab_init);
```

Initializes the SLUB slab allocator for kernel object allocations.

#### 8.6: Maple Tree

```c
aerosync_core_init(maple_tree_init);
```

Initializes the maple tree data structure (used for VMA management).

#### 8.7: VMA Cache

```c
aerosync_core_init(vma_cache_init);
```

Initializes the VMA (Virtual Memory Area) slab cache.

#### 8.8: Radix Tree

```c
aerosync_core_init(radix_tree_init);
```

Initializes the radix tree data structure (used for page cache, IDA).

---

### Phase 9: Per-CPU Data Structures

```c
aerosync_core_init(setup_per_cpu_areas);
aerosync_core_init(rcu_init);
aerosync_core_init(smp_prepare_boot_cpu);
aerosync_core_init(pmm_init_cpu);
aerosync_core_init(vmalloc_init);
```

**Purpose:** Set up per-CPU memory areas and initialize CPU-local data.

**Functions:**
- `setup_per_cpu_areas()` - Allocates and maps per-CPU regions
- `rcu_init()` - Initializes Read-Copy-Update mechanism
- `smp_prepare_boot_cpu()` - Prepares BSP for SMP operation
- `pmm_init_cpu()` - Initializes CPU-specific PMM data
- `vmalloc_init()` - Sets up virtual memory allocator

**Configuration:**
- `CONFIG_DYNAMIC_PER_CPU` - Enables dynamic per-CPU allocation
- `CONFIG_PER_CPU_CHUNK_SIZE` - Size of each per-CPU chunk (default: 64 KB)

---

### Phase 10: Kernel Symbol Table Finalization

```c
aerosync_extra_init(ksymtab_finalize);
```

Finalizes the kernel symbol table after all early initializations.

---

### Phase 11: Architecture-Specific Initialization (x86_64)

#### 11.1: Global Descriptor Table (GDT)

```c
aerosync_core_init(gdt_init);
```

Initializes the x86_64 Global Descriptor Table.

#### 11.2: Interrupt Descriptor Table (IDT)

```c
aerosync_core_init(idt_install);
```

Installs the Interrupt Descriptor Table for exceptions and interrupts.

#### 11.3: System Call Interface

```c
aerosync_core_init(syscall_init);
```

Initializes the system call entry points (syscall/sysenter).

---

### Phase 12: Floating Point Unit (FPU) Initialization

```c
aerosync_core_init(fpu_init);
```

Configures the FPU/SSE/AVX units (CR0, CR4, MXCSR registers).

---

### Phase 13: PID Allocator

```c
aerosync_core_init(pid_allocator_init);
```

Initializes the PID allocation subsystem for task_struct identification.

---

### Phase 14: Scheduler Initialization

```c
aerosync_core_init(sched_init);
bsp_task.active_mm = &init_mm;
aerosync_core_init(sched_init_task, &bsp_task);
```

**Purpose:** Initialize the scheduler and set up the boot processor task.

**Functions:**
- `sched_init()` - Initializes scheduler data structures (runqueues, etc.)
- `sched_init_task()` - Associates the BSP task with the initial memory context

**Task Structure:**
- `bsp_task` - Statically allocated task_struct for the boot CPU
- `active_mm` - Set to `init_mm` (initial kernel memory descriptor)

---

### Phase 15: Limine Module Manager (Conditional)

```c
#ifdef CONFIG_LIMINE_MODULE_MANAGER
aerosync_core_init(lmm_register_prober, initramfs_cpio_prober);
aerosync_core_init(lmm_register_prober, lmm_fkx_prober);
aerosync_core_init(lmm_register_prober, lmm_asrx_prober);
aerosync_core_init(lmm_init, get_module_request()->response);
#endif
```

**Purpose:** Initialize the Limine Module Manager (LMM) for FKX and ASRX module loading.

**Configuration:**
- `CONFIG_LIMINE_MODULE_MANAGER` - Enable LMM (default: y)
- `CONFIG_LMM_PROBE_EXTENSION_FIRST` - Check file extensions first (default: y)
- `CONFIG_LMM_SORT_BY_PRIORITY` - Sort modules by priority (default: n)

**Probers:**
- `initramfs_cpio_prober` - Detects initramfs CPIO archives
- `lmm_fkx_prober` - Detects FKX (Fused Kernel eXtension) modules
- `lmm_asrx_prober` - Detects ASRX modules

---

### Phase 16: Virtual File System (VFS)

```c
aerosync_core_init(vfs_init);
```

Initializes the Virtual File System layer.

---

### Phase 17: Resource Domain Initialization

```c
aerosync_core_init(resdomain_init);
```

Initializes the Resource Domain subsystem for hierarchical resource accounting.

**Configuration:**
- `CONFIG_RESDOMAINS` - Enable ResDomains (default: y)
- `CONFIG_RESDOMAIN_CPU` - CPU controller (default: y)
- `CONFIG_RESDOMAIN_MEM` - Memory controller (default: y)
- `CONFIG_RESFS_MOUNT` - Auto-mount resfs (default: y)
- `CONFIG_RESFS_MOUNT_PATH` - Default mount path (default: "/runtime/res")

---

### Phase 18: Memory Management Tests (Conditional)

```c
#ifdef INCLUDE_MM_TESTS
if (cmdline_find_option_bool(current_cmdline, "mtest")) {
    pmm_test();
    vmm_test();
    slab_test();
    vma_test();
    vmalloc_test();
    vmalloc_dump();
    vm_obj_stress_test();
}
#endif
```

**Purpose:** Run memory management stress tests to verify correctness.

**Configuration:**
- `CONFIG_INCLUDE_MM_TESTS` - Include test code (default: y)
- Requires `mtest` on command line to execute

**Tests:**
- PMM allocation/free tests
- VMM mapping tests
- Slab allocator tests
- VMA operations tests
- Vmalloc tests
- VM object stress tests

---

### Phase 19: Firmware and Crypto Initialization

```c
aerosync_core_init(fw_init);
aerosync_core_init(crypto_init);
```

**Purpose:** Initialize firmware abstraction layer and cryptographic subsystem.

---

### Phase 20: FKX Module Loading

```c
aerosync_core_init(system_load_extensions);
```

**Purpose:** Load all FKX (Fused Kernel eXtension) modules provided by the bootloader.

**Implementation:**
```c
static int __late_init __noinline __sysv_abi system_load_extensions(void) {
    if (lmm_get_count() > 0) {
        lmm_for_each_module(LMM_TYPE_FKX, lmm_load_fkx_callback, nullptr);
        if (fkx_finalize_loading() != 0) {
            printk(KERN_ERR FKX_CLASS "Failed to finalize module loading\n");
            return -ENOSYS;
        }
    } else {
        printk(KERN_NOTICE FKX_CLASS "no modules found via LMM\n");
        return -ENOSYS;
    }
    return 0;
}
```

---

### Phase 21: Module Class Initialization (Printk & Panic)

```c
fkx_init_module_class(FKX_PRINTK_CLASS);
fkx_init_module_class(FKX_PANIC_HANDLER_CLASS);
aerosync_core_init_exprcall(printk_init_late(), printk_late);
panic_handler_install();
```

**Purpose:** Initialize printk and panic handler module classes.

**Functions:**
- `printk_init_late()` - Completes printk initialization with full buffer support
- `panic_handler_install()` - Re-installs panic handler with full capabilities

---

### Phase 22: Interrupt Controller Class Initialization

```c
fkx_init_module_class(FKX_IC_CLASS);
aerosync_core_init(ic_register_lapic_get_id_early);
```

**Purpose:** Initialize interrupt controller module class and register early LAPIC ID retrieval.

---

### Phase 23: Early ACPICA Initialization

```c
aerosync_core_init(acpica_kernel_init_early);
```

Initializes early ACPICA kernel support.

---

### Phase 24: ACPI Table Initialization

```c
aerosync_core_init(acpi_tables_init);
```

Parses and initializes ACPI tables via uACPI.

---

### Phase 25: Interrupt Controller Installation

```c
interrupt_controller_t ic_type;
aerosync_core_init_status_ret(ic_install, ic_type);
acpica_notify_ic_ready();
```

**Purpose:** Install the appropriate interrupt controller (x2APIC/xAPIC).

**Return:** `ic_type` - Type of installed interrupt controller (used for SMP decision).

---

### Phase 26: Timer Subsystem Initialization

```c
fkx_init_module_class(FKX_TIMER_CLASS);
aerosync_core_init(time_init);
aerosync_core_init(time_calibrate_tsc_system);
aerosync_core_init(timer_init_subsystem);
```

**Purpose:** Initialize the unified time subsystem and calibrate TSC.

**Functions:**
- `time_init()` - Initializes time subsystem, selects best time source
- `time_calibrate_tsc_system()` - Recalibrates TSC using the best available source
- `timer_init_subsystem()` - Initializes timer framework (timers, alarms)

**Output:**
- TSC frequency in Hz (from CPUID or calibration)
- Selected time source name

---

### Phase 27: Late uACPI Initialization

```c
aerosync_core_init(acpica_kernel_init_late);
aerosync_extra_init(acpi_power_init);
aerosync_core_init(acpi_bus_enumerate);
```

**Purpose:** Complete uACPI initialization and enumerate ACPI devices.

**Functions:**
- `acpica_kernel_init_late()` - Completes uACPI initialization
- `acpi_power_init()` - Initializes ACPI power management (buttons, sleep states)
- `acpi_bus_enumerate()` - Enumerates ACPI device tree

**Configuration:**
- `CONFIG_ACPI_POWER_BUTTON` - Enable power button support (default: y)
- `CONFIG_ACPI_SLEEP_BUTTON` - Enable sleep button support (default: y)

---

### Phase 28: Driver Class Initialization

```c
fkx_init_module_class(FKX_DRIVER_CLASS);
```

Initializes all loaded driver modules.

---

### Phase 29: Device Tree Dump (Conditional)

```c
#ifdef CONFIG_LOG_DEVICE_TREE
if (cmdline_find_option_bool(current_cmdline, "dumpdevtree"))
    dump_device_tree();
#endif
```

**Configuration:**
- `CONFIG_LOG_DEVICE_TREE` - Enable device tree logging
- Requires `dumpdevtree` on command line

---

### Phase 30: SMP Initialization (Conditional)

```c
aerosync_core_init(smp_init, ic_type);
```

**Purpose:** Initialize Symmetric Multiprocessing support.

**Condition:** Only executed if `ic_type == INTC_APIC` (APIC-based interrupt controller).

**Configuration:**
- `CONFIG_SYMMETRIC_MP` - Enable SMP support (default: y)
- `CONFIG_MAX_CPUS` - Maximum number of CPUs (default: 512)

---

### Phase 31: SoftIRQ Initialization

```c
aerosync_core_init(softirq_init);
```

Initializes the softirq (software interrupt) subsystem.

---

### Phase 32: Asynchronous Printk (Conditional)

```c
#ifdef ASYNC_PRINTK
aerosync_core_init(printk_init_async);
#endif
```

**Purpose:** Enable high-performance asynchronous printk logging.

**Configuration:**
- `CONFIG_ASYNC_PRINTK` - Enable async printk (default: y)

---

### Phase 33: Kernel Init Thread Creation

```c
struct task_struct *init_task =
    kthread_create(kernel_init, nullptr, "kernel_init");
unmet_cond_crit(!init_task);
kthread_run(init_task);

cpu_sti();  // Enable interrupts

idle_loop();
__unreachable();
```

**Purpose:** Spawn the `kernel_init` kernel thread and enter the idle loop.

**Flow:**
1. Create `kernel_init` kernel thread
2. Assert thread creation succeeded (panic on failure)
3. Start the thread via `kthread_run()`
4. Enable interrupts with `cpu_sti()`
5. BSP enters `idle_loop()` (scheduler idle)

**Post-condition:** The BSP is now the idle thread. The `kernel_init` thread continues initialization.

---

## Secondary Initialization: `kernel_init()` Thread

```c
static int __late_init __noreturn __noinline __sysv_abi kernel_init(void *unused)
```

**Location:** `init/main.c`

**Description:** Kernel thread that performs late-stage initialization after SMP and interrupt subsystems are active.

**Constraints:**
- Runs as a kernel thread (can sleep, block)
- Executes after interrupts are enabled
- Never returns (becomes idle on completion)

### Initialization Sequence

#### 1. Generic Module Class

```c
printk(KERN_INFO KERN_CLASS "finishing system initialization\n");
fkx_init_module_class(FKX_GENERIC_CLASS);
```

Initializes generic FKX modules.

#### 2. AeroSync Core Initialization

```c
aerosync_core_init(rcu_spawn_kthreads);
```

Spawns RCU kthreads for callback processing.

**Configuration:**
- `CONFIG_RCU_PERCPU_TEST` - If set and `rcutest` on cmdline, runs RCU/per-CPU tests

#### 3. Memory Management Daemons

```c
aerosync_core_init(zmm_init);
aerosync_core_init(shm_init);
aerosync_core_init(kswapd_init);
aerosync_core_init(kcompactd_init);
aerosync_core_init(khugepaged_init);
aerosync_core_init(vm_writeback_init);
aerosync_core_init(kvmap_purged_init);
```

**Functions:**
- `zmm_init()` - Zone memory management initialization
- `shm_init()` - Shared memory subsystem
- `kswapd_init()` - Page swapping daemon
- `kcompactd_init()` - Memory compaction daemon
- `khugepaged_init()` - Transparent huge page daemon
- `vm_writeback_init()` - VM writeback mechanisms
- `kvmap_purged_init()` - Purged VMAP management

#### 4. Memory Hardening (Conditional)

```c
#ifdef MM_HARDENING
aerosync_core_init(mm_scrubber_init);
#endif
```

**Configuration:** `MM_HARDENING` - Enable memory scrubbing for security

#### 5. ASRX Module Loading

```c
aerosync_extra_init(system_load_modules);
```

Loads ASRX modules (if present).

#### 6. Init Process Execution

```c
printk(KERN_DEBUG KERN_CLASS "attempting to run init process: %s\n", STRINGIFY(CONFIG_INIT_PATH));
const int ret = run_init_process(STRINGIFY(CONFIG_INIT_PATH));
if (ret < 0) {
    printk(KERN_ERR KERN_CLASS "failed to execute %s. (%s)\n", STRINGIFY(CONFIG_INIT_PATH), errname(ret));
    printkln(KERN_ERR KERN_CLASS "attempted to kill init.");
}
```

**Purpose:** Execute the first userspace process (init).

**Configuration:**
- `CONFIG_INIT_PATH` - Path to init process (default: "/resources/binaries/init")

**Behavior:**
- If `run_init_process()` fails, logs error and continues to idle
- On success, this thread is replaced by the init process

#### 7. Hardware Information Dump

```c
fw_dump_hardware_info();
```

Dumps collected hardware information (CPUs, memory, firmware).

#### 8. Completion

```c
printkln(KERN_CLASS "AeroSync global initialization done.");

while (1) {
    idle_loop();
}
```

Logs completion message and enters idle loop if init execution failed.

---

## Initialization Macros Reference

| Macro                                        | Description                                      |
|----------------------------------------------|--------------------------------------------------|
| `aerosync_core_init(fn, ...)`                | Initialize core subsystem (mandatory)            |
| `aerosync_core_init_exprcall(fn, result)`    | Initialize with expression call, capture result  |
| `aerosync_core_init_status_ret(fn, result)`  | Initialize with status return                    |
| `aerosync_extra_init(fn, ...)`               | Initialize extra subsystem (optional, may fail)  |
| `__init`                                     | Function/data only used during initialization    |
| `__late_init`                                | Late initialization function                     |
| `__noreturn`                                 | Function does not return                         |
| `__noinline`                                 | Prevent inlining (for stack traces)              |
| `__sysv_abi`                                 | System V ABI calling convention                  |
| `__no_sanitize`                              | Disable sanitizer instrumentation                |

for implementation see [compiler.h](../include/compiler.h) and [errno.h](../include/aerosync/errno.h)

---

## Initialization Order Summary

```
start_kernel()
├── Panic handler setup
├── Bootloader verification
├── Early console & TSC
├── Command line parsing
├── Version display
├── Ksymtab init
├── Memory management (PMM, VMM, slab, trees)
├── Per-CPU areas & RCU
├── Architecture init (GDT, IDT, syscall, FPU)
├── Scheduler & BSP task
├── Module manager (LMM)
├── VFS & ResDomain
├── MM tests (conditional)
├── Firmware & crypto
├── FKX module loading
├── Module classes (printk, panic, IC, timer, driver)
├── uACPI (early → tables → IC → late)
├── Time subsystem & TSC calibration
├── ACPI power & bus enumeration
├── SMP initialization
├── SoftIRQ
├── Async printk (conditional)
└── kernel_init thread spawn
    └── BSP → idle_loop()

kernel_init()
├── Generic module class
├── RCU kthreads
├── Memory daemons (zmm, shm, kswapd, kcompactd, khugepaged, writeback)
├── Memory hardening (conditional)
├── ASRX module loading
├── Init process execution
├── Hardware info dump
└── Idle loop
```

---

## See Also

- `init/main.c` - Primary initialization implementation
- `arch/x86_64/` - Architecture-specific initialization
- `mm/` - Memory management subsystem
- `aerosync/` - Core kernel subsystems
- `drivers/acpi/` - ACPI driver and power management
- `doc/kconfig.md` - Kconfig configuration system
- `doc/cmdline.md` - Kernel command line options
