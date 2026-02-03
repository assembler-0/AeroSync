/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/acpi/uacpi_glue.c
 * @brief uACPI kernel glue layer
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <aerosync/classes.h>
#include <aerosync/mutex.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/wait.h>
#include <aerosync/signal.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/irq.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/requests.h>
#include <arch/x86_64/tsc.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <uacpi/kernel_api.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>

static volatile int s_ic_ready = 0;
// Pending ACPI IRQ handlers before IC is ready
typedef struct pending_irq_install {
  uacpi_u32 irq;
  uacpi_interrupt_handler handler;
  uacpi_handle ctx;
  struct pending_irq_install *next;
} pending_irq_install_t;

static pending_irq_install_t *s_pending_head = nullptr;
static pending_irq_install_t *s_pending_tail = nullptr;

int uacpi_kernel_init_early(void) {
  uacpi_status ret = uacpi_initialize(0);
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR ACPI_CLASS "uACPI initialization failed: %s\n",
           uacpi_status_to_string(ret));
    return -1;
  }
  printk(KERN_INFO ACPI_CLASS "uACPI initialized\n");

  ret = uacpi_namespace_load();
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR ACPI_CLASS "uACPI namespace load failed: %s\n",
           uacpi_status_to_string(ret));
    return -1;
  }
  printk(KERN_INFO ACPI_CLASS "uACPI namespace loaded\n");
  return 0;
}

int uacpi_kernel_init_late(void) {
  uacpi_status ret = uacpi_namespace_initialize();
  if (uacpi_unlikely_error(ret)) {
    printk(KERN_ERR ACPI_CLASS "uACPI namespace init failed: %s\n",
           uacpi_status_to_string(ret));
    return -1;
  }
  printk(ACPI_CLASS "uACPI namespace initialized\n");
  return 0;
}

/*
 * RSDP
 */
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
  if (!get_rsdp_request()->response || !get_rsdp_request()->response->address) {
    return UACPI_STATUS_NOT_FOUND;
  }

  // Limine provides a virtual address (mapped in HHDM usually or lower half
  // identity). We need the physical address. We can use vmm_virt_to_phys. Note:
  // rsdp_request.response->address is a void*.

  uint64_t virt = (uint64_t)get_rsdp_request()->response->address;
  *out_rsdp_address = vmm_virt_to_phys(&init_mm, virt);

  // Fallback if virt_to_phys fails (returns 0) but address is non-null?
  // Usually 0 is valid physical, but vmm_virt_to_phys returns 0 on error?
  // Assuming it works.

  return UACPI_STATUS_OK;
}

/*
 * Memory Management
 */
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
  /*
   * We use standard ioremap (Uncacheable) here.
   * While ACPI tables are in RAM and could be WB, uACPI also uses this
   * for MMIO registers. UC is the safe default for registers.
   */
  return ioremap(addr, len);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
  (void)len;
  iounmap(addr);
}

void *uacpi_kernel_alloc(uacpi_size size) {
  return size > SLAB_MAX_SIZE ? vmalloc(size) : kmalloc(size);
}

void *uacpi_kernel_alloc_zeroed(uacpi_size size) {
  void *res = uacpi_kernel_alloc(size);
  if (res)
    memset(res, 0, size);
  return res;
}

void uacpi_kernel_free(void *mem) {
  if (!mem)
    return;
  const uint64_t addr = (uint64_t)mem;
  (addr >= VMALLOC_VIRT_BASE && addr < VMALLOC_VIRT_END)
  ? vfree(mem)
  : kfree(mem);
}

/*
 * Logging
 */
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *str) {
  if (!str || *str == '\0')
    return;

  // Check if string contains only newline
  if (str[0] == '\n' && str[1] == '\0')
    return;

  switch (level) {
  case UACPI_LOG_DEBUG:
    printk(KERN_DEBUG ACPI_CLASS "%s", str);
    break;
  case UACPI_LOG_TRACE:
    printk(KERN_DEBUG ACPI_CLASS "%s", str);
    break;
  case UACPI_LOG_INFO:
    printk(KERN_INFO ACPI_CLASS "%s", str);
    break;
  case UACPI_LOG_WARN:
    printk(KERN_WARNING ACPI_CLASS "%s", str);
    break;
  case UACPI_LOG_ERROR:
    printk(KERN_ERR ACPI_CLASS "%s", str);
    break;
  default:
    printk(KERN_INFO ACPI_CLASS "%s", str);
    break;
  }
}

/*
 * PCI
 */
#include <drivers/pci/pci.h>

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle) {
  pci_handle_t *h = kmalloc(sizeof(pci_handle_t));
  if (!h)
    return UACPI_STATUS_OUT_OF_MEMORY;
  h->segment = address.segment;
  h->bus = address.bus;
  h->device = address.device;
  h->function = address.function;
  *out_handle = h;
  return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) { kfree(handle); }

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset,
                                    uacpi_u8 *value) {
  *value = (uacpi_u8)pci_read((pci_handle_t *)device, offset, 8);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
                                     uacpi_u16 *value) {
  *value = (uacpi_u16)pci_read((pci_handle_t *)device, offset, 16);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
                                     uacpi_u32 *value) {
  *value = (uacpi_u32)pci_read((pci_handle_t *)device, offset, 32);
  return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset,
                                     uacpi_u8 value) {
  pci_write((pci_handle_t *)device, offset, value, 8);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
                                      uacpi_u16 value) {
  pci_write((pci_handle_t *)device, offset, value, 16);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
                                      uacpi_u32 value) {
  pci_write((pci_handle_t *)device, offset, value, 32);
  return UACPI_STATUS_OK;
}

/*
 * IO
 */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle) {
  /*
   * On x86, SystemIO space is the I/O port space, accessed via in/out.
   * We don't need to map it; the "handle" is just the port address.
   */
  (void)len;
  *out_handle = (uacpi_handle)(uintptr_t)base;
  return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
  (void)handle;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
                                   uacpi_u8 *out_value) {
  *out_value = inb((uint64_t)handle + offset);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u16 *out_value) {
  *out_value = inw((uint64_t)handle + offset);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u32 *out_value) {
  *out_value = inl((uint64_t)handle + offset);
  return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 in_value) {
  outb((uint64_t)handle + offset, in_value);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 in_value) {
  outw((uint64_t)handle + offset, in_value);
  return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 in_value) {
  outl((uint64_t)handle + offset, in_value);
  return UACPI_STATUS_OK;
}

/*
 * Time
 */
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
  return get_time_ns();
}

void uacpi_kernel_stall(uacpi_u8 usec) { tsc_delay(usec * 1000); }

void uacpi_kernel_sleep(uacpi_u64 msec) { tsc_delay_ms(msec); }


/*
 * Sync / Mutex / Event
 */
typedef struct {
  mutex_t mutex;
} uacpi_mutex_obj_t;

typedef struct {
  wait_queue_head_t wait_q;
  volatile int counter;
  spinlock_t lock;
} uacpi_event_obj_t;

uacpi_handle uacpi_kernel_create_mutex(void) {
  uacpi_mutex_obj_t *obj = kmalloc(sizeof(uacpi_mutex_obj_t));
  if (obj) {
    mutex_init(&obj->mutex);
  }
  return obj;
}
void uacpi_kernel_free_mutex(uacpi_handle h) { kfree(h); }

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout) {
  uacpi_mutex_obj_t *obj = h;

  if (timeout == 0) {
    return mutex_trylock(&obj->mutex) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
  }

  if (timeout == 0xFFFF) {
    mutex_lock(&obj->mutex);
    return UACPI_STATUS_OK;
  }

  uint64_t start = get_time_ns();
  uint64_t limit = (uint64_t)timeout * 1000000;

  while (1) {
    if (mutex_trylock(&obj->mutex))
      return UACPI_STATUS_OK;
    
    uint64_t now = get_time_ns();
    if (now - start >= limit)
      return UACPI_STATUS_TIMEOUT;
    
    uint64_t remaining = limit - (now - start);
    // Sleep for a short while or remaining time
    uint64_t sleep_ns = remaining > 10000000 ? 10000000 : remaining;
    
    current->state = TASK_UNINTERRUPTIBLE;
    schedule_timeout(sleep_ns);
  }
}

void uacpi_kernel_release_mutex(uacpi_handle h) {
  uacpi_mutex_obj_t *obj = h;
  mutex_unlock(&obj->mutex);
}

uacpi_handle uacpi_kernel_create_event(void) {
  uacpi_event_obj_t *obj = kmalloc(sizeof(uacpi_event_obj_t));
  if (obj) {
    init_waitqueue_head(&obj->wait_q);
    obj->counter = 0;
    spinlock_init(&obj->lock);
  }
  return obj;
}
void uacpi_kernel_free_event(uacpi_handle h) { kfree(h); }

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout) {
  uacpi_event_obj_t *obj = h;
  wait_queue_entry_t wait;
  init_wait(&wait);

  uint64_t start = get_time_ns();
  uint64_t limit = (timeout == 0xFFFF) ? 0 : (uint64_t)timeout * 1000000;

  while (1) {
    irq_flags_t flags = spinlock_lock_irqsave(&obj->lock);
    if (obj->counter > 0) {
      obj->counter--;
      spinlock_unlock_irqrestore(&obj->lock, flags);
      finish_wait(&obj->wait_q, &wait);
      return UACPI_TRUE;
    }

    if (timeout == 0) {
      spinlock_unlock_irqrestore(&obj->lock, flags);
      finish_wait(&obj->wait_q, &wait);
      return UACPI_FALSE;
    }

    if (timeout != 0xFFFF) {
      uint64_t now = get_time_ns();
      if (now - start >= limit) {
        printk(KERN_WARNING ACPI_CLASS "Event timeout: %u ms (start=%llu, now=%llu, diff=%llu)\n", 
               timeout, start, now, now - start);
        spinlock_unlock_irqrestore(&obj->lock, flags);
        finish_wait(&obj->wait_q, &wait);
        return UACPI_FALSE;
      }

      prepare_to_wait(&obj->wait_q, &wait, TASK_UNINTERRUPTIBLE);
      spinlock_unlock_irqrestore(&obj->lock, flags);
      
      uint64_t remaining = limit - (now - start);
      schedule_timeout(remaining);
    } else {
      prepare_to_wait(&obj->wait_q, &wait, TASK_UNINTERRUPTIBLE);
      spinlock_unlock_irqrestore(&obj->lock, flags);
      schedule();
    }
  }
}

void uacpi_kernel_signal_event(uacpi_handle h) {
  uacpi_event_obj_t *obj = h;
  irq_flags_t flags = spinlock_lock_irqsave(&obj->lock);
  obj->counter++;
  wake_up_nr(&obj->wait_q, 1);
  spinlock_unlock_irqrestore(&obj->lock, flags);
}

void uacpi_kernel_reset_event(uacpi_handle h) {
  uacpi_event_obj_t *obj = h;
  irq_flags_t flags = spinlock_lock_irqsave(&obj->lock);
  obj->counter = 0;
  spinlock_unlock_irqrestore(&obj->lock, flags);
}

/*
 * Firmware Request
 */
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
  switch (req->type) {
  case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
    printk(ACPI_CLASS "Breakpoint: %s\n",
           req->breakpoint.ctx ? req->breakpoint.ctx : "No context");
    return UACPI_STATUS_OK;
  case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
    printk(KERN_ERR ACPI_CLASS "Fatal: Type %x Code %x Arg %x\n",
           req->fatal.type, req->fatal.code, req->fatal.arg);
    // Maybe panic?
    return UACPI_STATUS_OK;
  }
  return UACPI_STATUS_OK;
}

/*
 * Thread ID
 */
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
  return (uacpi_thread_id)get_current();
}

/*
 * Spinlocks
 */
uacpi_handle uacpi_kernel_create_spinlock(void) {
  spinlock_t *l = kmalloc(sizeof(spinlock_t));
  if (l)
    spinlock_init(l);
  return (void *)l;
}
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
  // We need to save flags. spinlock_lock doesn't return flags usually.
  // spinlock.h: spinlock_lock(spinlock_t *lock)
  // We should implement spinlock_lock_irqsave behavior.
  // If spinlock.h doesn't have irqsave, we do it manually.
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  spinlock_lock(h);
  return (uacpi_cpu_flags)flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags) {
  spinlock_unlock(h);
  restore_irq_flags((irq_flags_t)flags);
}

/*
 * Interrupts
 */
typedef struct irq_mapping {
  uacpi_interrupt_handler handler;
  uacpi_handle ctx;
  uint32_t vector;
  struct irq_mapping *next;
} irq_mapping_t;

static irq_mapping_t *irq_map_head = nullptr;
static DEFINE_SPINLOCK(irq_map_lock);

// Generic trampoline
static void acpi_irq_trampoline(cpu_regs *regs) {
  uint32_t vector = regs->interrupt_number;

  irq_flags_t flags = spinlock_lock_irqsave(&irq_map_lock);
  irq_mapping_t *curr = irq_map_head;
  while (curr) {
    if (curr->vector == vector) {
      curr->handler(curr->ctx);
    }
    curr = curr->next;
  }
  spinlock_unlock_irqrestore(&irq_map_lock, flags);

  ic_send_eoi(vector - 32); // Send EOI.
}

void uacpi_notify_ic_ready(void) {
  // Mark IC available and flush any pending ACPI IRQ installs
  s_ic_ready = 1;

  pending_irq_install_t *p = s_pending_head;
  while (p) {
    uint32_t vector = p->irq + 32;

    // Create mapping node
    irq_mapping_t *map = kmalloc(sizeof(irq_mapping_t));
    if (map) {
      map->handler = p->handler;
      map->ctx = p->ctx;
      map->vector = vector;

      irq_flags_t flags = spinlock_lock_irqsave(&irq_map_lock);
      map->next = irq_map_head;
      irq_map_head = map;
      spinlock_unlock_irqrestore(&irq_map_lock, flags);

      // Install trampoline and unmask
      irq_install_handler(vector, acpi_irq_trampoline);
      ic_enable_irq(p->irq);
    }

    pending_irq_install_t *next = p->next;
    kfree(p);
    p = next;
  }
  s_pending_head = s_pending_tail = nullptr;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {
  if (!s_ic_ready) {
    // Defer until IC is ready
    pending_irq_install_t *node = kmalloc(sizeof(pending_irq_install_t));
    if (!node)
      return UACPI_STATUS_OUT_OF_MEMORY;
    node->irq = irq;
    node->handler = handler;
    node->ctx = ctx;
    node->next = nullptr;
    if (s_pending_tail)
      s_pending_tail->next = node;
    else
      s_pending_head = node;
    s_pending_tail = node;
    if (out_irq_handle)
      *out_irq_handle = node; // temporary handle
    return UACPI_STATUS_OK;
  }

  uint32_t vector = irq + 32; // Map GSI to Vector (PIC offset)

  irq_mapping_t *map = kmalloc(sizeof(irq_mapping_t));
  if (!map)
    return UACPI_STATUS_OUT_OF_MEMORY;

  map->handler = handler;
  map->ctx = ctx;
  map->vector = vector;

  // Add to list
  irq_flags_t flags = spinlock_lock_irqsave(&irq_map_lock);
  map->next = irq_map_head;
  irq_map_head = map;
  spinlock_unlock_irqrestore(&irq_map_lock, flags);

  if (out_irq_handle)
    *out_irq_handle = map;

  // Install the trampoline for this vector
  irq_install_handler(vector, acpi_irq_trampoline);

  // Unmask
  ic_enable_irq(irq);

  return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                         uacpi_handle irq_handle) {
  (void)handler;
  irq_mapping_t *target = irq_handle;

  irq_flags_t flags = spinlock_lock_irqsave(&irq_map_lock);
  // Remove from list
  if (irq_map_head == target) {
    irq_map_head = target->next;
  } else {
    irq_mapping_t *curr = irq_map_head;
    while (curr && curr->next != target)
      curr = curr->next;
    if (curr)
      curr->next = target->next;
  }
  spinlock_unlock_irqrestore(&irq_map_lock, flags);

  // If no more handlers for this vector, uninstall trampoline?
  // We can just leave it or check count.

  kfree(target);
  return UACPI_STATUS_OK;
}

/*
 * Work Scheduling
 */
typedef struct work_item {
  uacpi_work_handler handler;
  uacpi_handle ctx;
  struct work_item *next;
} work_item_t;

static work_item_t *work_head = nullptr;
static work_item_t *work_tail = nullptr;
static DEFINE_SPINLOCK(work_lock);
static wait_queue_head_t work_wait_q;

static int acpi_worker_thread(void *data) {
  (void)data;
  wait_queue_entry_t wait;
  init_wait(&wait);

  while (1) {
    work_item_t *work = nullptr;

    spinlock_lock(&work_lock);
    if (work_head) {
      work = work_head;
      work_head = work->next;
      if (!work_head)
        work_tail = nullptr;
    }
    spinlock_unlock(&work_lock);

    if (work) {
      work->handler(work->ctx);
      kfree(work);
    } else {
      prepare_to_wait(&work_wait_q, &wait, TASK_UNINTERRUPTIBLE);

      /* Re-check if work arrived while we were preparing to wait */
      spinlock_lock(&work_lock);
      if (work_head) {
        spinlock_unlock(&work_lock);
        finish_wait(&work_wait_q, &wait);
        continue;
      }
      spinlock_unlock(&work_lock);

      schedule();
      finish_wait(&work_wait_q, &wait);
    }
  }
  return 0;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle ctx) {
  (void)type;
  work_item_t *work = kmalloc(sizeof(work_item_t));
  if (!work)
    return UACPI_STATUS_OUT_OF_MEMORY;

  work->handler = handler;
  work->ctx = ctx;
  work->next = nullptr;

  spinlock_lock(&work_lock);
  if (work_tail) {
    work_tail->next = work;
    work_tail = work;
  } else {
    work_head = work_tail = work;
  }
  spinlock_unlock(&work_lock);

  wake_up(&work_wait_q);

  return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
  // Wait until queue is empty?
  // This is called before shutdown or similar.
  while (1) {
    spinlock_lock(&work_lock);
    if (!work_head) {
      spinlock_unlock(&work_lock);
      break;
    }
    spinlock_unlock(&work_lock);
    tsc_delay_ms(10);
  }
  return UACPI_STATUS_OK;
}

static struct task_struct *worker = nullptr;

/*
 * Initialization
 */
uacpi_status uacpi_kernel_initialize(uacpi_init_level current_init_lvl) {
  if (current_init_lvl == UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED) {
    spinlock_init(&work_lock);
    init_waitqueue_head(&work_wait_q);
    // Start worker thread
    worker = kthread_create(acpi_worker_thread, nullptr, "acpi_worker");
    kthread_run(worker);
  }
  return UACPI_STATUS_OK;
}

void uacpi_kernel_deinitialize(void) {
  send_signal(SIGKILL, worker);
}

#include <aerosync/fkx/fkx.h>
EXPORT_SYMBOL(uacpi_table_find_by_signature);
EXPORT_SYMBOL(uacpi_status_to_string);
EXPORT_SYMBOL(uacpi_table_unref);
EXPORT_SYMBOL(uacpi_for_each_subtable);
