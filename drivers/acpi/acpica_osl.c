/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/acpica_osl.c
 * @brief ACPICA OS Services Layer (OSL) implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <aerosync/sysintf/acpica.h>
#include <aerosync/classes.h>
#include <aerosync/mutex.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/wait.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/time.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/irq.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/requests.h>
#include <arch/x86_64/tsc.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>


/* --- OSL Initialization --- */
ACPI_STATUS
AcpiOsInitialize(
  void) {
  return AE_OK;
}

ACPI_STATUS
AcpiOsTerminate(
  void) {
  return AE_OK;
}

/* --- Table interfaces --- */
ACPI_PHYSICAL_ADDRESS
AcpiOsGetRootPointer(
  void) {
  if (!get_rsdp_request()->response || !get_rsdp_request()->response->address) {
    return 0;
  }
  uint64_t virt = (uint64_t) get_rsdp_request()->response->address;
  return vmm_virt_to_phys(&init_mm, virt);
}

ACPI_STATUS
AcpiOsPredefinedOverride(
  const ACPI_PREDEFINED_NAMES *InitVal,
  ACPI_STRING *NewVal) {
  *NewVal = nullptr;
  return AE_OK;
}

ACPI_STATUS
AcpiOsTableOverride(
  ACPI_TABLE_HEADER *ExistingTable,
  ACPI_TABLE_HEADER **NewTable) {
  *NewTable = nullptr;
  return AE_OK;
}

ACPI_STATUS
AcpiOsPhysicalTableOverride(
  ACPI_TABLE_HEADER *ExistingTable,
  ACPI_PHYSICAL_ADDRESS *NewAddress,
  UINT32 *NewTableLength) {
  *NewAddress = 0;
  return AE_OK;
}

/* --- Spinlock primitives --- */
ACPI_STATUS
AcpiOsCreateLock(
  ACPI_SPINLOCK *OutHandle) {
  spinlock_t *lock = kmalloc(sizeof(spinlock_t));
  if (!lock)
    return AE_NO_MEMORY;
  spinlock_init(lock);
  *OutHandle = (ACPI_SPINLOCK) lock;
  return AE_OK;
}

void
AcpiOsDeleteLock(
  ACPI_SPINLOCK Handle) {
  kfree(Handle);
}

ACPI_CPU_FLAGS
AcpiOsAcquireLock(
  ACPI_SPINLOCK Handle) {
  return spinlock_lock_irqsave((spinlock_t *) Handle);
}

void
AcpiOsReleaseLock(
  ACPI_SPINLOCK Handle,
  ACPI_CPU_FLAGS Flags) {
  spinlock_unlock_irqrestore((spinlock_t *) Handle, Flags);
}

/* --- Semaphore primitives --- */
typedef struct {
  wait_queue_head_t wait_q;
  volatile uint32_t counter;
  uint32_t max_units;
  spinlock_t lock;
} acpi_osl_semaphore_t;

ACPI_STATUS
AcpiOsCreateSemaphore(
  UINT32 MaxUnits,
  UINT32 InitialUnits,
  ACPI_SEMAPHORE *OutHandle) {
  acpi_osl_semaphore_t *sem = kmalloc(sizeof(acpi_osl_semaphore_t));
  if (!sem)
    return AE_NO_MEMORY;

  init_waitqueue_head(&sem->wait_q);
  sem->counter = InitialUnits;
  sem->max_units = MaxUnits;
  spinlock_init(&sem->lock);

  *OutHandle = (ACPI_SEMAPHORE) sem;
  return AE_OK;
}

ACPI_STATUS
AcpiOsDeleteSemaphore(
  ACPI_SEMAPHORE Handle) {
  if (!Handle)
    return AE_BAD_PARAMETER;
  kfree(Handle);
  return AE_OK;
}

ACPI_STATUS
AcpiOsWaitSemaphore(
  ACPI_SEMAPHORE Handle,
  UINT32 Units,
  UINT16 Timeout) {
  acpi_osl_semaphore_t *sem = (acpi_osl_semaphore_t *) Handle;
  if (!sem)
    return AE_BAD_PARAMETER;

  wait_queue_entry_t wait;
  init_wait(&wait);

  uint64_t start = get_time_ns();
  uint64_t limit = (Timeout == 0xFFFF) ? 0 : (uint64_t) Timeout * 1000000;

  while (1) {
    irq_flags_t flags = spinlock_lock_irqsave(&sem->lock);
    if (sem->counter >= Units) {
      sem->counter -= Units;
      spinlock_unlock_irqrestore(&sem->lock, flags);
      finish_wait(&sem->wait_q, &wait);
      return AE_OK;
    }

    if (Timeout == 0) {
      spinlock_unlock_irqrestore(&sem->lock, flags);
      finish_wait(&sem->wait_q, &wait);
      return AE_TIME;
    }

    if (Timeout != 0xFFFF) {
      uint64_t now = get_time_ns();
      if (now - start >= limit) {
        spinlock_unlock_irqrestore(&sem->lock, flags);
        finish_wait(&sem->wait_q, &wait);
        return AE_TIME;
      }

      prepare_to_wait(&sem->wait_q, &wait, TASK_UNINTERRUPTIBLE);
      spinlock_unlock_irqrestore(&sem->lock, flags);

      uint64_t remaining = limit - (now - start);
      schedule_timeout(remaining);
    } else {
      prepare_to_wait(&sem->wait_q, &wait, TASK_UNINTERRUPTIBLE);
      spinlock_unlock_irqrestore(&sem->lock, flags);
      schedule();
    }
  }
}

ACPI_STATUS
AcpiOsSignalSemaphore(
  ACPI_SEMAPHORE Handle,
  UINT32 Units) {
  acpi_osl_semaphore_t *sem = (acpi_osl_semaphore_t *) Handle;
  if (!sem)
    return AE_BAD_PARAMETER;

  irq_flags_t flags = spinlock_lock_irqsave(&sem->lock);
  sem->counter += Units;
  wake_up_nr(&sem->wait_q, Units);
  spinlock_unlock_irqrestore(&sem->lock, flags);

  return AE_OK;
}

/* --- Mutex primitives --- */
ACPI_STATUS
AcpiOsCreateMutex(
  ACPI_MUTEX *OutHandle) {
  mutex_t *m = kmalloc(sizeof(mutex_t));
  if (!m)
    return AE_NO_MEMORY;
  mutex_init(m);
  *OutHandle = (ACPI_MUTEX) m;
  return AE_OK;
}

void
AcpiOsDeleteMutex(
  ACPI_MUTEX Handle) {
  if (!Handle)
    return;
  kfree(Handle);
}

ACPI_STATUS
AcpiOsAcquireMutex(
  ACPI_MUTEX Handle,
  UINT16 Timeout) {
  mutex_t *m = (mutex_t *) Handle;
  if (!m)
    return AE_BAD_PARAMETER;

  if (Timeout == 0) {
    return mutex_trylock(m) ? AE_OK : AE_TIME;
  }

  if (Timeout == 0xFFFF) {
    mutex_lock(m);
    return AE_OK;
  }

  uint64_t start = get_time_ns();
  uint64_t limit = (uint64_t) Timeout * 1000000;

  while (1) {
    if (mutex_trylock(m))
      return AE_OK;

    uint64_t now = get_time_ns();
    if (now - start >= limit)
      return AE_TIME;

    uint64_t remaining = limit - (now - start);
    uint64_t sleep_ns = remaining > 10000000 ? 10000000 : remaining;

    current->state = TASK_UNINTERRUPTIBLE;
    schedule_timeout(sleep_ns);
  }
}

void
AcpiOsReleaseMutex(
  ACPI_MUTEX Handle) {
  if (!Handle)
    return;
  mutex_unlock((mutex_t *) Handle);
}

/* --- Memory allocation --- */
void *
AcpiOsAllocate(
  ACPI_SIZE Size) {
  return Size > SLAB_MAX_SIZE ? vmalloc(Size) : kmalloc(Size);
}

void *
AcpiOsAllocateZeroed(
  ACPI_SIZE Size) {
  return Size > SLAB_MAX_SIZE ? vzalloc(Size) : kzalloc(Size);
}

void
AcpiOsFree(
  void *Memory) {
  if (!Memory)
    return;
  const uint64_t addr = (uint64_t) Memory;
  (addr >= VMALLOC_VIRT_BASE && addr < VMALLOC_VIRT_END)
    ? vfree(Memory)
    : kfree(Memory);
}

void *
AcpiOsMapMemory(
  ACPI_PHYSICAL_ADDRESS Where,
  ACPI_SIZE Length) {
  return ioremap(Where, Length);
}

void
AcpiOsUnmapMemory(
  void *LogicalAddress,
  ACPI_SIZE Size) {
  (void) Size;
  iounmap(LogicalAddress);
}

ACPI_STATUS
AcpiOsGetPhysicalAddress(
  void *LogicalAddress,
  ACPI_PHYSICAL_ADDRESS *PhysicalAddress) {
  *PhysicalAddress = vmm_virt_to_phys(&init_mm, (uintptr_t) LogicalAddress);
  return *PhysicalAddress ? AE_OK : AE_ERROR;
}

/* --- Cache --- */
ACPI_STATUS
AcpiOsCreateCache(
  char *CacheName,
  UINT16 ObjectSize,
  UINT16 MaxDepth,
  ACPI_CACHE_T **ReturnCache) {
  (void) MaxDepth;
  kmem_cache_t *cache = kmem_cache_create(CacheName, ObjectSize, 0, 0);
  if (!cache)
    return AE_NO_MEMORY;

  *ReturnCache = (ACPI_CACHE_T) cache;
  return AE_OK;
}

ACPI_STATUS
AcpiOsDeleteCache(
  ACPI_CACHE_T *Cache) {
  (void) Cache;
  /* kmem_cache_destroy not implemented in kernel yet */
  return AE_OK;
}

ACPI_STATUS
AcpiOsPurgeCache(
  ACPI_CACHE_T *Cache) {
  (void) Cache;
  return AE_OK;
}

void *
AcpiOsAcquireObject(
  ACPI_CACHE_T *Cache) {
  if (!Cache)
    return nullptr;
  return kmem_cache_zalloc((kmem_cache_t *) Cache);
}

ACPI_STATUS
AcpiOsReleaseObject(
  ACPI_CACHE_T *Cache,
  void *Object) {
  if (!Cache || !Object)
    return AE_BAD_PARAMETER;
  kmem_cache_free((kmem_cache_t *) Cache, Object);
  return AE_OK;
}

/* --- Interrupts --- */
typedef struct acpi_irq_mapping {
  ACPI_OSD_HANDLER handler;
  void *ctx;
  uint32_t vector;
  struct acpi_irq_mapping *next;
} acpi_irq_mapping_t;

static acpi_irq_mapping_t *acpi_irq_map_head = nullptr;
static DEFINE_SPINLOCK(acpi_irq_map_lock);
static volatile int acpica_ic_ready = 0;

typedef struct pending_acpi_irq_install {
  uint32_t irq;
  ACPI_OSD_HANDLER handler;
  void *ctx;
  struct pending_acpi_irq_install *next;
} pending_acpi_irq_install_t;

static pending_acpi_irq_install_t *s_pending_acpi_irq_head = nullptr;
static pending_acpi_irq_install_t *s_pending_acpi_irq_tail = nullptr;

static void __no_cfi acpica_irq_trampoline(cpu_regs *regs) {
  uint32_t vector = regs->interrupt_number;

  irq_flags_t flags = spinlock_lock_irqsave(&acpi_irq_map_lock);
  acpi_irq_mapping_t *curr = acpi_irq_map_head;
  while (curr) {
    if (curr->vector == vector) {
      curr->handler(curr->ctx);
    }
    curr = curr->next;
  }
  spinlock_unlock_irqrestore(&acpi_irq_map_lock, flags);

  ic_send_eoi(vector - 32);
}

void acpica_notify_ic_ready(void) {
  acpica_ic_ready = 1;

  pending_acpi_irq_install_t *p = s_pending_acpi_irq_head;
  while (p) {
    uint32_t vector = p->irq + 32;

    acpi_irq_mapping_t *map = kmalloc(sizeof(acpi_irq_mapping_t));
    if (map) {
      map->handler = p->handler;
      map->ctx = p->ctx;
      map->vector = vector;

      irq_flags_t flags = spinlock_lock_irqsave(&acpi_irq_map_lock);
      map->next = acpi_irq_map_head;
      acpi_irq_map_head = map;
      spinlock_unlock_irqrestore(&acpi_irq_map_lock, flags);

      irq_install_handler(vector, acpica_irq_trampoline);
      ic_enable_irq(p->irq);
    }

    pending_acpi_irq_install_t *next = p->next;
    kfree(p);
    p = next;
  }
  s_pending_acpi_irq_head = s_pending_acpi_irq_tail = nullptr;
}

ACPI_STATUS
AcpiOsInstallInterruptHandler(
  UINT32 InterruptNumber,
  ACPI_OSD_HANDLER ServiceRoutine,
  void *Context) {
  if (!acpica_ic_ready) {
    pending_acpi_irq_install_t *node = kmalloc(sizeof(pending_acpi_irq_install_t));
    if (!node)
      return AE_NO_MEMORY;
    node->irq = InterruptNumber;
    node->handler = ServiceRoutine;
    node->ctx = Context;
    node->next = nullptr;
    if (s_pending_acpi_irq_tail)
      s_pending_acpi_irq_tail->next = node;
    else
      s_pending_acpi_irq_head = node;
    s_pending_acpi_irq_tail = node;
    return AE_OK;
  }

  uint32_t vector = InterruptNumber + 32;
  acpi_irq_mapping_t *map = kmalloc(sizeof(acpi_irq_mapping_t));
  if (!map)
    return AE_NO_MEMORY;

  map->handler = ServiceRoutine;
  map->ctx = Context;
  map->vector = vector;

  irq_flags_t flags = spinlock_lock_irqsave(&acpi_irq_map_lock);
  map->next = acpi_irq_map_head;
  acpi_irq_map_head = map;
  spinlock_unlock_irqrestore(&acpi_irq_map_lock, flags);

  irq_install_handler(vector, acpica_irq_trampoline);
  ic_enable_irq(InterruptNumber);

  return AE_OK;
}

ACPI_STATUS
AcpiOsRemoveInterruptHandler(
  UINT32 InterruptNumber,
  ACPI_OSD_HANDLER ServiceRoutine) {
  (void) InterruptNumber;

  irq_flags_t flags = spinlock_lock_irqsave(&acpi_irq_map_lock);
  acpi_irq_mapping_t *curr = acpi_irq_map_head;
  acpi_irq_mapping_t *prev = nullptr;

  while (curr) {
    if (curr->handler == ServiceRoutine) {
      if (prev)
        prev->next = curr->next;
      else
        acpi_irq_map_head = curr->next;

      spinlock_unlock_irqrestore(&acpi_irq_map_lock, flags);
      kfree(curr);
      return AE_OK;
    }
    prev = curr;
    curr = curr->next;
  }
  spinlock_unlock_irqrestore(&acpi_irq_map_lock, flags);

  return AE_NOT_FOUND;
}

/* --- Threads and Scheduling --- */
ACPI_THREAD_ID
AcpiOsGetThreadId(
  void) {
  return (ACPI_THREAD_ID)get_current();
}

typedef struct acpi_osl_work_item {
  ACPI_OSD_EXEC_CALLBACK function;
  void *context;
  struct acpi_osl_work_item *next;
} acpi_osl_work_item_t;

static acpi_osl_work_item_t *acpi_osl_work_head = nullptr;
static acpi_osl_work_item_t *acpi_osl_work_tail = nullptr;
static DEFINE_SPINLOCK(acpi_osl_work_lock);
static wait_queue_head_t acpi_osl_work_wait_q;
static struct task_struct *acpi_osl_worker = nullptr;

static int __no_cfi acpica_worker_thread(void *data) {
  (void) data;
  wait_queue_entry_t wait;
  init_wait(&wait);

  while (1) {
    acpi_osl_work_item_t *work = nullptr;

    spinlock_lock(&acpi_osl_work_lock);
    if (acpi_osl_work_head) {
      work = acpi_osl_work_head;
      acpi_osl_work_head = work->next;
      if (!acpi_osl_work_head)
        acpi_osl_work_tail = nullptr;
    }
    spinlock_unlock(&acpi_osl_work_lock);

    if (work) {
      work->function(work->context);
      kfree(work);
    } else {
      prepare_to_wait(&acpi_osl_work_wait_q, &wait, TASK_UNINTERRUPTIBLE);

      spinlock_lock(&acpi_osl_work_lock);
      if (acpi_osl_work_head) {
        spinlock_unlock(&acpi_osl_work_lock);
        finish_wait(&acpi_osl_work_wait_q, &wait);
        continue;
      }
      spinlock_unlock(&acpi_osl_work_lock);

      schedule();
      finish_wait(&acpi_osl_work_wait_q, &wait);
    }
  }
  return 0;
}

ACPI_STATUS
AcpiOsExecute(
  ACPI_EXECUTE_TYPE Type,
  ACPI_OSD_EXEC_CALLBACK Function,
  void *Context) {
  acpi_osl_work_item_t *work = kmalloc(sizeof(acpi_osl_work_item_t));
  if (!work)
    return AE_NO_MEMORY;

  work->function = Function;
  work->context = Context;
  work->next = nullptr;

  spinlock_lock(&acpi_osl_work_lock);
  if (acpi_osl_work_tail) {
    acpi_osl_work_tail->next = work;
    acpi_osl_work_tail = work;
  } else {
    acpi_osl_work_head = acpi_osl_work_tail = work;
  }
  spinlock_unlock(&acpi_osl_work_lock);

  wake_up(&acpi_osl_work_wait_q);
  return AE_OK;
}

void
AcpiOsWaitEventsComplete(
  void) {
  while (1) {
    spinlock_lock(&acpi_osl_work_lock);
    if (!acpica_ic_ready || !acpi_osl_work_head) {
      spinlock_unlock(&acpi_osl_work_lock);
      break;
    }
    spinlock_unlock(&acpi_osl_work_lock);
    tsc_delay_ms(10);
  }
}

void
AcpiOsSleep(
  UINT64 Milliseconds) {
  current->state = TASK_UNINTERRUPTIBLE;
  schedule_timeout(Milliseconds * 1000000);
}

void
AcpiOsStall(
  UINT32 Microseconds) {
  delay_us(Microseconds);
}

/* --- I/O interfaces --- */
ACPI_STATUS
AcpiOsReadPort(
  ACPI_IO_ADDRESS Address,
  UINT32 *Value,
  UINT32 Width) {
  switch (Width) {
    case 8:
      *Value = inb(Address);
      break;
    case 16:
      *Value = inw(Address);
      break;
    case 32:
      *Value = inl(Address);
      break;
    default:
      return AE_BAD_PARAMETER;
  }
  return AE_OK;
}

ACPI_STATUS
AcpiOsWritePort(
  ACPI_IO_ADDRESS Address,
  UINT32 Value,
  UINT32 Width) {
  switch (Width) {
    case 8:
      outb(Address, (uint8_t) Value);
      break;
    case 16:
      outw(Address, (uint16_t) Value);
      break;
    case 32:
      outl(Address, (uint32_t) Value);
      break;
    default:
      return AE_BAD_PARAMETER;
  }
  return AE_OK;
}

/* --- Memory interfaces --- */
ACPI_STATUS
AcpiOsReadMemory(
  ACPI_PHYSICAL_ADDRESS Address,
  UINT64 *Value,
  UINT32 Width) {
  void *ptr = ioremap(Address, Width / 8);
  if (!ptr)
    return AE_NO_MEMORY;

  switch (Width) {
    case 8:
      *Value = *(volatile uint8_t *) ptr;
      break;
    case 16:
      *Value = *(volatile uint16_t *) ptr;
      break;
    case 32:
      *Value = *(volatile uint32_t *) ptr;
      break;
    case 64:
      *Value = *(volatile uint64_t *) ptr;
      break;
    default:
      iounmap(ptr);
      return AE_BAD_PARAMETER;
  }

  iounmap(ptr);
  return AE_OK;
}

ACPI_STATUS
AcpiOsWriteMemory(
  ACPI_PHYSICAL_ADDRESS Address,
  UINT64 Value,
  UINT32 Width) {
  void *ptr = ioremap(Address, Width / 8);
  if (!ptr)
    return AE_NO_MEMORY;

  switch (Width) {
    case 8:
      *(volatile uint8_t *) ptr = (uint8_t) Value;
      break;
    case 16:
      *(volatile uint16_t *) ptr = (uint16_t) Value;
      break;
    case 32:
      *(volatile uint32_t *) ptr = (uint32_t) Value;
      break;
    case 64:
      *(volatile uint64_t *) ptr = Value;
      break;
    default:
      iounmap(ptr);
      return AE_BAD_PARAMETER;
  }

  iounmap(ptr);
  return AE_OK;
}

/* --- PCI --- */
#include <drivers/pci/pci.h>

ACPI_STATUS
AcpiOsReadPciConfiguration(
  ACPI_PCI_ID *PciId,
  UINT32 Reg,
  UINT64 *Value,
  UINT32 Width) {
  pci_handle_t h = {
    .segment = PciId->Segment,
    .bus = (uint8_t) PciId->Bus,
    .device = (uint8_t) PciId->Device,
    .function = (uint8_t) PciId->Function
  };
  *Value = pci_read(&h, Reg, (uint8_t) Width);
  return AE_OK;
}

ACPI_STATUS
AcpiOsWritePciConfiguration(
  ACPI_PCI_ID *PciId,
  UINT32 Reg,
  UINT64 Value,
  UINT32 Width) {
  pci_handle_t h = {
    .segment = PciId->Segment,
    .bus = (uint8_t) PciId->Bus,
    .device = (uint8_t) PciId->Device,
    .function = (uint8_t) PciId->Function
  };
  pci_write(&h, Reg, Value, (uint8_t) Width);
  return AE_OK;
}

/* --- Miscellaneous --- */
BOOLEAN
AcpiOsReadable(
  void *Pointer,
  ACPI_SIZE Length) {
  return (uintptr_t) Pointer >= 0xFFFF800000000000;
}

BOOLEAN
AcpiOsWritable(
  void *Pointer,
  ACPI_SIZE Length) {
  return (uintptr_t) Pointer >= 0xFFFF800000000000;
}

UINT64
AcpiOsGetTimer(
  void) {
  return get_time_ns() / 100;
}

ACPI_STATUS
AcpiOsSignal(
  UINT32 Function,
  void *Info) {
  switch (Function) {
    case ACPI_SIGNAL_FATAL: {
      ACPI_SIGNAL_FATAL_INFO *fatal = (ACPI_SIGNAL_FATAL_INFO *) Info;
      panic("ACPI Fatal: Type %x Code %x Arg %x", fatal->Type, fatal->Code, fatal->Argument);
      break;
    }
    case ACPI_SIGNAL_BREAKPOINT:
      printk(KERN_DEBUG ACPI_CLASS "Breakpoint: %s\n", (char *) Info);
      break;
  }
  return AE_OK;
}

ACPI_STATUS
AcpiOsEnterSleep(
  UINT8 SleepState,
  UINT32 RegaValue,
  UINT32 RegbValue) {
  return AE_OK;
}

/* --- Debug print --- */
void ACPI_INTERNAL_VAR_XFACE
AcpiOsPrintf(
  const char *Format,
  ...) {
  va_list args;
  va_start(args, Format);
  char buff[1024];
  vscnprintf(buff, sizeof(buff), Format, args);
  printk(KERN_DEBUG ACPI_CLASS "%s", buff);
  va_end(args);
}

void
AcpiOsVprintf(
  const char *Format,
  va_list Args) {
  char buff[1024];
  vscnprintf(buff, sizeof(buff), Format, Args);
  printk(KERN_DEBUG ACPI_CLASS "%s", buff);
}

void
AcpiOsRedirectOutput(
  void *Destination) {
}

/* --- Debug IO --- */
ACPI_STATUS
AcpiOsGetLine(
  char *Buffer,
  UINT32 BufferLength,
  UINT32 *BytesRead) {
  return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS
AcpiOsInitializeDebugger(
  void) {
  return AE_OK;
}

void
AcpiOsTerminateDebugger(
  void) {
}

ACPI_STATUS
AcpiOsWaitCommandReady(
  void) {
  return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS
AcpiOsNotifyCommandComplete(
  void) {
  return AE_NOT_IMPLEMENTED;
}

void
AcpiOsTracePoint(
  ACPI_TRACE_EVENT_TYPE Type,
  BOOLEAN Begin,
  UINT8 *Aml,
  char *Pathname) {
}

/* --- Table Access --- */
ACPI_STATUS
AcpiOsGetTableByName(
  char *Signature,
  UINT32 Instance,
  ACPI_TABLE_HEADER **Table,
  ACPI_PHYSICAL_ADDRESS *Address) {
  return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS
AcpiOsGetTableByIndex(
  UINT32 Index,
  ACPI_TABLE_HEADER **Table,
  UINT32 *Instance,
  ACPI_PHYSICAL_ADDRESS *Address) {
  return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS
AcpiOsGetTableByAddress(
  ACPI_PHYSICAL_ADDRESS Address,
  ACPI_TABLE_HEADER **Table) {
  return AE_NOT_IMPLEMENTED;
}

static ACPI_STATUS
osl_osi_handler(ACPI_STRING interface_name, UINT32 *supported)
{
  if (!supported || !interface_name) return AE_BAD_PARAMETER;

  /* TODO: add `acpi_osi=` boot parameter support here */
  /* FIXME: discover the actual needed 'lies', not blindly supporting all */

  /* Default to not supported */
  *supported = 0;

  /* Whitelist what we actually support */
  if (!strcmp(interface_name, "Windows 2013") ||
      !strcmp(interface_name, "Windows 2015") ||
      !strcmp(interface_name, "Windows 2019") ||
      !strcmp(interface_name, "Windows 2022") ||
      !strcmp(interface_name, "Windows 2025") ||
      !strcmp(interface_name, "Windows 2012") ||
      !strcmp(interface_name, "Windows 2009") ||
      !strcmp(interface_name, "Module Device")  ||
      !strcmp(interface_name, "Processor Device") ||
      !strcmp(interface_name, "3.0 Thermal Model") ||
      !strcmp(interface_name, "Extended Address Space Descriptor")) {
    *supported = 1;
    return AE_OK;
  }

#ifdef CONFIG_DENY_LINUX_OSI
  /*
   * Explicitly deny "Linux" - some vendor firmware (Lenovo infamously)
   * returns broken thermal/battery data when this returns true
   */
  if (!strcmp(interface_name, "Linux")) {
    *supported = 0;
    return AE_OK;
  }
#endif

  return AE_OK;
}

/* --- Initialization Calls --- */

int acpica_kernel_init_early(void) {
  ACPI_STATUS status;

  printk(KERN_INFO ACPI_CLASS "ACPICA (R) - Copyright (c) 1999 - 2025 Intel Corp\n");

  status = AcpiInitializeSubsystem();
  if (ACPI_FAILURE(status)) {
    printk(KERN_ERR ACPI_CLASS "Could not initialize ACPICA: %s\n", AcpiFormatException(status));
    return -ENODEV;
  }

  status = AcpiInitializeTables(nullptr, 16, FALSE);
  if (ACPI_FAILURE(status)) {
    printk(KERN_ERR ACPI_CLASS "Could not initialize tables: %s\n", AcpiFormatException(status));
    return -ENODEV;
  }

  status = AcpiInstallInterfaceHandler((ACPI_INTERFACE_HANDLER)osl_osi_handler);
  if (ACPI_FAILURE(status)) {
    printk(KERN_WARNING ACPI_CLASS "Failed to install OSI handler: %s\n", AcpiFormatException(status));
  }

  status = AcpiLoadTables();
  if (ACPI_FAILURE(status)) {
    printk(KERN_ERR ACPI_CLASS "Could not load tables: %s\n", AcpiFormatException(status));
    return -ENODEV;
  }

  printk(KERN_INFO ACPI_CLASS "ACPICA early initialization complete\n");
  return 0;
}

int acpica_kernel_init_late(void) {
  ACPI_STATUS status;

  spinlock_init(&acpi_osl_work_lock);
  init_waitqueue_head(&acpi_osl_work_wait_q);
  acpi_osl_worker = kthread_create(acpica_worker_thread, nullptr, "acpi_worker");
  kthread_run(acpi_osl_worker);

  status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
  if (ACPI_FAILURE(status)) {
    printk(KERN_ERR ACPI_CLASS "Could not enable subsystem: %s\n", AcpiFormatException(status));
    return -ENODEV;
  }

  status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
  if (ACPI_FAILURE(status)) {
    printk(KERN_ERR ACPI_CLASS "Could not initialize objects: %s\n", AcpiFormatException(status));
    return -ENODEV;
  }

  printk(KERN_INFO ACPI_CLASS "ACPICA late initialization complete\n");
  return 0;
}
