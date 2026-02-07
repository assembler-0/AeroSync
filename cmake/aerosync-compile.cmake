# ============================================================================
# Kernel Linking
# ============================================================================
add_executable(aerosync.krnl
    ${AEROSYNC_SOURCES}
    ${FKX_PUB_HEADER}
)

add_dependencies(aerosync.krnl fkx_key_header)

target_compile_options(aerosync.krnl PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -m64
        -target ${CLANG_TARGET_TRIPLE}
        -O${OPT_LEVEL}
        -g${DSYM_LEVEL}
        -fdata-sections
        -ffunction-sections
        -fno-omit-frame-pointer
        -finline-functions
        -foptimize-sibling-calls
        -mno-implicit-float
        -msoft-float
        -mno-red-zone
        -mserialize
        -ffreestanding
        -nostdlib
        -fno-pic
        -fno-pie
        -mcmodel=kernel
        -fvisibility=hidden
    >
    $<$<COMPILE_LANGUAGE:ASM_NASM>:
        -felf64
    >
)

if(STACK_PROTECTION_ALL)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fstack-protector-all
            -D_FORTIFY_SOURCE=2
        >
    )
endif()

if(STACK_PROTECTION)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fstack-protector-strong
        >
    )
endif()

if(INTEL_CET)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fcf-protection=full
        >
    )
endif()

if (LTO)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -flto
        >
    )
endif()

if(SANITIZER)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fsanitize=undefined,bounds,null,return,vla-bound
        >
    )
endif()

set(AEROSYNC_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/aerosync.ld" CACHE STRING "AeroSync linker script")

# Configure the linker to use ld.lld with proper arguments
set_target_properties(aerosync.krnl PROPERTIES
    LINK_DEPENDS "${AEROSYNC_LINKER_SCRIPT}"
)

# Set linker flags for this specific target
target_link_options(aerosync.krnl PRIVATE
    -fuse-ld=lld
    -T ${AEROSYNC_LINKER_SCRIPT}
    -nostdlib
    -static
    -Wl,-melf_x86_64
)

if (LTO)
    target_link_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -flto
            -Wl,--gc-sections
        >
    )
endif()
