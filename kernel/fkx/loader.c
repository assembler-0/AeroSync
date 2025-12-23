#include <kernel/fkx/fkx.h>
#include <kernel/fkx/elf_parser.h>
#include <mm/slab.h>
#include <mm/vmalloc.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/io.h>
#include <drivers/timer/time.h>
#include <drivers/apic/ic.h>
#include <kernel/classes.h>
#include <kernel/panic.h>

#define FKX_DEBUG 1

extern volatile struct limine_framebuffer_response* get_framebuffer_response(void);

// Global API Table
static struct fkx_kernel_api g_fkx_api = {
    .version = FKX_API_VERSION,
    .kmalloc = kmalloc,
    .kfree = kfree,
    .vmalloc = vmalloc,
    .vmalloc_exec = vmalloc_exec,
    .vfree = vfree,
    .viomap = viomap,
    .viounmap = viounmap,
    .vmm_map_page = vmm_map_page,
    .vmm_unmap_page = vmm_unmap_page,
    .vmm_virt_to_phys = vmm_virt_to_phys,
    .vmm_switch_pml4 = vmm_switch_pml4,
    .memset = memset,
    .memcpy = memcpy,
    .memmove = memmove,
    .memcmp = memcmp,
    .strlen = strlen,
    .strcpy = strcpy,
    .strcmp = strcmp,
    .printk = printk,
    .snprintf = snprintf,
    .panic = panic,
    .pmm_alloc_page = pmm_alloc_page,
    .pmm_free_page = pmm_free_page,
    .pmm_alloc_pages = pmm_alloc_pages,
    .pmm_free_pages = pmm_free_pages,
    .pmm_phys_to_virt = pmm_phys_to_virt,
    .pmm_virt_to_phys = pmm_virt_to_phys,
    .inb = inb,
    .inw = inw,
    .inl = inl,
    .outb = outb,
    .outw = outw,
    .outl = outl,
    .ndelay = time_wait_ns,
    .udelay = delay_us,
    .mdelay = delay_ms,
    .sdelay = delay_s,
    .get_time_ns = get_time_ns,
    .rdtsc = rdtsc,
    .time_register_source = time_register_source,
    .ic_register_controller = ic_register_controller,
    .ic_shutdown_controller = ic_shutdown_controller,
    .ic_enable_irq = ic_enable_irq,
    .ic_disable_irq = ic_disable_irq,
    .ic_send_eoi = ic_send_eoi,
    .ic_set_timer = ic_set_timer,
    .ic_get_frequency = ic_get_frequency,
    .ic_send_ipi = ic_send_ipi,
    .ic_get_controller_type = ic_get_controller_type,
    .get_framebuffer_response = get_framebuffer_response,
    .printk_register_backend = printk_register_backend,
    .printk_set_sink = printk_set_sink,
    .printk_shutdown = printk_shutdown,
    .spinlock_init = spinlock_init,
    .spinlock_lock = spinlock_lock,
    .spinlock_unlock = spinlock_unlock,
    .spinlock_lock_irqsave = spinlock_lock_irqsave,
    .spinlock_unlock_irqrestore = spinlock_unlock_irqrestore
};

int fkx_load_module(void *data, size_t size) {
    if (!elf_verify(data, size)) {
        printk(KERN_ERR FKX_CLASS "Invalid ELF magic or architecture\n");
        return -1;
    }

    Elf64_Ehdr *hdr = (Elf64_Ehdr *)data;
    
    // We only support ET_DYN (Shared Object) for now
    if (hdr->e_type != ET_DYN) {
        printk(KERN_ERR FKX_CLASS "Module must be ET_DYN (PIE/Shared Object)\n");
        return -1;
    }

    // 1. Calculate memory requirements
    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;
    
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)data + hdr->e_phoff);
    
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
            if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        }
    }
    
    size_t total_size = max_vaddr - min_vaddr;
    if (total_size == 0) {
        printk(KERN_ERR FKX_CLASS "No loadable segments found\n");
        return -1;
    }

    // 2. Allocate memory
    void *base = vmalloc_exec(total_size);
    if (!base) {
        printk(KERN_ERR FKX_CLASS "Failed to allocate memory for module\n");
        return -1;
    }
    
    uint64_t base_addr = (uint64_t)base;
    
    // 3. Load segments
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            void *dest = (void *)(base_addr + (phdrs[i].p_vaddr - min_vaddr));
            void *src = (void *)((uint8_t *)data + phdrs[i].p_offset);
            
            // Copy file content
            if (phdrs[i].p_filesz > 0) {
                memcpy(dest, src, phdrs[i].p_filesz);
            }
            
            // Zero out BSS
            if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                memset((void *)((uint64_t)dest + phdrs[i].p_filesz), 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
            }
        }
    }
    
    // 4. Apply Relocations
    Elf64_Shdr *sections = (Elf64_Shdr *)((uint8_t *)data + hdr->e_shoff);
    
    for (int i = 0; i < hdr->e_shnum; i++) {
        if (sections[i].sh_type == SHT_RELA) {
             Elf64_Rela *relas = (Elf64_Rela *)((uint8_t *)data + sections[i].sh_offset);
             size_t count = sections[i].sh_size / sizeof(Elf64_Rela);
             
             // The section being relocated
             Elf64_Shdr *target_sec = &sections[sections[i].sh_info];
             // Base address of the target section in memory
             // Note: For ET_DYN, relocations are usually relative to the load base if they refer to addresses.
             // But r_offset is virtual address in the image.
             // We need to translate r_offset to our loaded address.
             
             for (size_t j = 0; j < count; j++) {
                 uint64_t r_offset = relas[j].r_offset;
                 uint64_t *target = (uint64_t *)(base_addr + (r_offset - min_vaddr));
                 
                 uint64_t type = ELF64_R_TYPE(relas[j].r_info);
                 int64_t addend = relas[j].r_addend;
                 
                 switch (type) {
                     case R_X86_64_RELATIVE:
                         *target = base_addr + addend;
                         break;
                     // Implement others if symbols are involved, but for self-contained modules RELATIVE is key
                     default:
                         // We ignore others for now as we assume strict self-containment or relative addressing
                         // Real implementation would look up symbols in the dynsym
                         break;
                 }
             }
        }
    }

    // 5. Find Module Info
    const Elf64_Shdr *info_sec = elf_get_section(data, ".fkx_info");
    if (!info_sec) {
        printk(KERN_ERR FKX_CLASS ".fkx_info section not found\n");
        vfree(base);
        return -1;
    }
    
    // The info section is loaded in memory at (base + info_sec->sh_addr)
    // assuming it was in a LOAD segment. If not, we might need to read from file data.
    // Usually section headers describe where it *should* be. 
    // If it's SHF_ALLOC, it's in memory.
    
    struct fkx_module_info *info = NULL;
    
    if (info_sec->sh_flags & SHF_ALLOC) {
         info = (struct fkx_module_info *)(base_addr + (info_sec->sh_addr - min_vaddr));
    } else {
         // It's not in a loaded segment? That's weird for .fkx_info, but let's support reading from raw data
         info = (struct fkx_module_info *)((uint8_t *)data + info_sec->sh_offset);
    }

    if (info->magic != FKX_MAGIC) {
        printk(KERN_ERR FKX_CLASS "Invalid module magic: %x\n", info->magic);
        vfree(base);
        return -1;
    }
    
    printk(FKX_CLASS "Loading module '%s' v%s by %s\n", info->name, info->version, info->author);
    
    // 6. Fixup Entry Point
    // The init function pointer in 'info' is the link-time address. 
    // We need to adjust it by base_addr.
    
    // However, if we applied relocations correctly, and 'info' is in a writable segment, 
    // R_X86_64_RELATIVE relocations should have already fixed 'info->init' IF it was a pointer in data.
    // But 'info' is a struct. info.init is a function pointer. 
    // The compiler usually emits a relocation for this pointer.
    // So *if* we processed R_X86_64_RELATIVE, info->init should point to the correct address in `base`.
    
    // Let's verify if info->init looks like a valid address in our range
    // If not (e.g. it's still a low offset), maybe relocations failed or weren't present.
    // But we trust the relocations.
    
    // 7. Call Init
    if (info->init) {
        int ret = info->init(&g_fkx_api);
        if (ret != 0) {
            printk(KERN_ERR FKX_CLASS "Module init failed: %d\n", ret);
            vfree(base);
            return ret;
        }
    }
    
    printk(FKX_CLASS "Module '%s' loaded successfully\n", info->name);
    return 0;
}
