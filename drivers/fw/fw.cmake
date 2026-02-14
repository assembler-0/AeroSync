set(FW_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/fw)

set(FW_SOURCES
        ${FW_SOURCE_DIR}/fw.c
        ${FW_SOURCE_DIR}/smbios.c
        ${FW_SOURCE_DIR}/nvram.c
        ${FW_SOURCE_DIR}/efi.c
)

add_fkx_module(fw ${FW_SOURCES})