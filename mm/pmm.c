#include <mm/pmm.h>
#include <drivers/uart/serial.h>
#include <kernel/panic.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/spinlock.h>

// Linker symbols defining the actual kernel physical range (including BSS)
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];

static uint8_t *bitmap = NULL;
static uint64_t bitmap_size = 0;
static uint64_t highest_page = 0;
static uint64_t used_memory = 0;  // Tracks used RAM pages * PAGE_SIZE
static uint64_t total_memory = 0; // Tracks total usable RAM * PAGE_SIZE
static uint64_t free_memory = 0;  // Tracks currently free RAM * PAGE_SIZE

// Synchronization and Optimization
static volatile int pmm_lock = 0;
static uint64_t last_free_index = 0;

static void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_unset(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int bitmap_test(uint64_t bit) {
    return bitmap[bit / 8] & (1 << (bit % 8));
}

void pmm_init(PXS_BOOT_INFO *boot_info) {
    printk("Initializing PMM...\n");

    EFI_MEMORY_DESCRIPTOR *map = boot_info->MemoryMap;
    uint64_t map_entries = boot_info->MemoryMapSize / boot_info->DescriptorSize;
    uint64_t descriptor_size = boot_info->DescriptorSize;

    // Sanitize Descriptor Size
    if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        descriptor_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    }

    // 1. Calculate total memory and highest physical address
    for (uint64_t i = 0; i < map_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + (i * descriptor_size));

        uint64_t end_addr = desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE);
        
        if (desc->Type == EfiConventionalMemory || 
            desc->Type == EfiLoaderCode || 
            desc->Type == EfiLoaderData || 
            desc->Type == EfiBootServicesCode || 
            desc->Type == EfiBootServicesData ||
            desc->Type == EfiACPIReclaimMemory) {
            total_memory += (desc->NumberOfPages * PAGE_SIZE);
            
            if (end_addr > (highest_page * PAGE_SIZE)) {
                highest_page = end_addr / PAGE_SIZE;
            }
        }
    }

    // 2. Calculate bitmap size
    bitmap_size = highest_page / 8;
    if (highest_page % 8) bitmap_size++;

    // 3. Find a place for the bitmap (First large enough Conventional Memory block)
    int bitmap_found = 0;
    
    // Use linker symbols for robust kernel protection (includes BSS)
    uint64_t kernel_start = (uint64_t)_kernel_phys_start;
    uint64_t kernel_end = (uint64_t)_kernel_phys_end;
    
    uint64_t initrd_start = boot_info->InitrdAddress;
    uint64_t initrd_end = initrd_start + boot_info->InitrdSize;
    
    // Protect Boot Info Struct
    uint64_t info_start = (uint64_t)boot_info;
    uint64_t info_end = info_start + sizeof(PXS_BOOT_INFO);

    // Protect Memory Map
    uint64_t map_start = (uint64_t)map;
    uint64_t map_end = map_start + boot_info->MemoryMapSize;

    for (uint64_t i = 0; i < map_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + (i * descriptor_size));
        
        if (desc->Type == EfiConventionalMemory) {
            uint64_t block_start = desc->PhysicalStart;
            uint64_t block_end = block_start + (desc->NumberOfPages * PAGE_SIZE);
            uint64_t candidate = block_start;

            // Helper macro to advance candidate if it overlaps with a range
            #define AVOID_RANGE(start, end) \
                if (candidate < (end) && (candidate + bitmap_size) > (start)) { \
                    candidate = ((end) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); \
                }

            AVOID_RANGE(kernel_start, kernel_end);
            
            if (boot_info->InitrdSize > 0) {
                AVOID_RANGE(initrd_start, initrd_end);
            }

            AVOID_RANGE(info_start, info_end);
            AVOID_RANGE(map_start, map_end);

            #undef AVOID_RANGE

            if (candidate < block_end && (candidate + bitmap_size) <= block_end) {
                bitmap = (uint8_t *)candidate;
                bitmap_found = 1;
                break;
            }
        }
    }

    if (!bitmap_found) {
        panic("PMM: Could not find memory for bitmap!\n");
    }

    // 4. Initialize bitmap: Mark all as used first
    // This assumes all memory is used/reserved unless explicitly freed later
    memset(bitmap, 0xFF, bitmap_size);
    
    // 5. Mark Conventional Memory as free
    free_memory = 0;
    for (uint64_t i = 0; i < map_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + (i * descriptor_size));
        
        if (desc->Type == EfiConventionalMemory) {
            uint64_t start_page = desc->PhysicalStart / PAGE_SIZE;
            uint64_t pages = desc->NumberOfPages;
            
            for (uint64_t j = 0; j < pages; j++) {
                if (start_page + j < highest_page) {
                    bitmap_unset(start_page + j);
                    free_memory += PAGE_SIZE;
                }
            }
        }
    }

    // 6. Mark Bitmap itself as used
    uint64_t bitmap_start_page = (uint64_t)bitmap / PAGE_SIZE;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;


    for (uint64_t i = 0; i < bitmap_pages; i++) {
        if (!bitmap_test(bitmap_start_page + i)) {
            bitmap_set(bitmap_start_page + i);
            free_memory -= PAGE_SIZE;
        }
    }
    
    // 7. Mark Kernel as used (using updated safe ranges)
    uint64_t kernel_start_page = kernel_start / PAGE_SIZE;
    uint64_t kernel_pages = (kernel_end - kernel_start + PAGE_SIZE - 1) / PAGE_SIZE;


    for (uint64_t i = 0; i < kernel_pages; i++) {
         if (!bitmap_test(kernel_start_page + i)) {
            bitmap_set(kernel_start_page + i);
            free_memory -= PAGE_SIZE;
        }
    }

    // 8. Mark Initrd as used
    if (boot_info->InitrdAddress != 0 && boot_info->InitrdSize != 0) {
        uint64_t initrd_start_page = boot_info->InitrdAddress / PAGE_SIZE;
        uint64_t initrd_pages = (boot_info->InitrdSize + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint64_t i = 0; i < initrd_pages; i++) {
            if (!bitmap_test(initrd_start_page + i)) {
                bitmap_set(initrd_start_page + i);
                free_memory -= PAGE_SIZE;
            }
        }
    }

    used_memory = total_memory - free_memory;
    last_free_index = 0; // Start search from beginning

    // Verification
    if (bitmap[0] == 0xFF && bitmap[1] == 0xFF) {
         printk("WARNING: Bitmap starts with 0xFFFF. Memory might not be free.\n");
    }
    printk("PMM: Physical Memory Manager initialized\n");
}

void *pmm_alloc_page() {
    irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

    // Search from last_free_index to end
    for (uint64_t i = last_free_index; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_memory -= PAGE_SIZE;
            used_memory += PAGE_SIZE;
            last_free_index = i + 1;
            spinlock_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(i * PAGE_SIZE);
        }
    }

    // Wrap around: Search from 0 to last_free_index
    for (uint64_t i = 0; i < last_free_index; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_memory -= PAGE_SIZE;
            used_memory += PAGE_SIZE;
            last_free_index = i + 1;
            spinlock_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(i * PAGE_SIZE);
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();

    irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

    // Basic search for contiguous pages (doesn't use wrap-around optimization for simplicity yet)
    // But we start from last_free_index to be smart.
    for (uint64_t i = last_free_index; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            int found = 1;
            for (size_t j = 1; j < count; j++) {
                if ((i + j) >= highest_page || bitmap_test(i + j)) {
                    found = 0;
                    i += j;
                    break;
                }
            }

            if (found) {
                for (size_t j = 0; j < count; j++) {
                    bitmap_set(i + j);
                }
                free_memory -= (count * PAGE_SIZE);
                used_memory += (count * PAGE_SIZE);
                last_free_index = i + count;
                spinlock_unlock_irqrestore(&pmm_lock, flags);
                return (void *)(i * PAGE_SIZE);
            }
        }
    }
    
    // Fallback search from 0 if above failed
    if (last_free_index > 0) {
         for (uint64_t i = 0; i < last_free_index; i++) {
            if (!bitmap_test(i)) {
                int found = 1;
                for (size_t j = 1; j < count; j++) {
                    if ((i + j) >= highest_page || bitmap_test(i + j)) {
                        found = 0;
                        i += j;
                        break;
                    }
                }

                if (found) {
                    for (size_t j = 0; j < count; j++) {
                        bitmap_set(i + j);
                    }
                    free_memory -= (count * PAGE_SIZE);
                    used_memory += (count * PAGE_SIZE);
                    last_free_index = i + count;
                    spinlock_unlock_irqrestore(&pmm_lock, flags);
                    return (void *)(i * PAGE_SIZE);
                }
            }
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void pmm_free_page(void *address) {
    uint64_t page = (uint64_t)address / PAGE_SIZE;
    
    if (page < highest_page) {
        irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);
        
        if (bitmap_test(page)) {
             bitmap_unset(page);
             free_memory += PAGE_SIZE;
             used_memory -= PAGE_SIZE;
             
             // Optimization: If we freed a page lower than our current search pointer,
             // update pointer so we pick this up next time.
             if (page < last_free_index) {
                 last_free_index = page;
             }
        }
        
        spinlock_unlock_irqrestore(&pmm_lock, flags);
    }
}

void pmm_free_pages(void *address, size_t count) {
    uint64_t page = (uint64_t)address / PAGE_SIZE;
    
    irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);
    
    for (size_t i = 0; i < count; i++) {
        if (page + i < highest_page) {
            if (bitmap_test(page + i)) {
                bitmap_unset(page + i);
                free_memory += PAGE_SIZE;
                used_memory -= PAGE_SIZE;
            }
        }
    }
    
    if (page < last_free_index) {
        last_free_index = page;
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_get_total_memory() {
    return total_memory;
}

uint64_t pmm_get_free_memory() {
    return free_memory;
}

uint64_t pmm_get_used_memory() {
    return used_memory;
}
