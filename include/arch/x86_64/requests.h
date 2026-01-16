#pragma once

#include <limine/limine.h>

volatile uint64_t *get_limine_base_revision();
volatile struct limine_bootinfo_request *get_bootinfo_request(void);
volatile struct limine_executable_cmdline_request *get_cmdline_request(void);
volatile struct limine_firmware_type_request *get_fw_request(void);
volatile struct limine_date_at_boot_request *get_date_at_boot_request(void);
volatile struct limine_module_request *get_module_request(void);
volatile struct limine_bootloader_info_request *get_bootloader_info_request(void);
volatile struct limine_bootloader_performance_request *get_bootloader_performance_request(void);
volatile struct limine_smbios_request *get_smbios_request(void);
volatile struct limine_hhdm_request *get_hhdm_request(void);
volatile struct limine_paging_mode_request *get_paging_request(void);
volatile struct limine_memmap_request *get_memmap_request(void);
volatile struct limine_framebuffer_request *get_framebuffer_request(void);
volatile struct limine_rsdp_request *get_rsdp_request(void);