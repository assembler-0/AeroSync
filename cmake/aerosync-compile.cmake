# ============================================================================
# AeroSync Linking & Compilation - Clang/LLVM
# ============================================================================

add_executable(aerosync.krnl
        ${AEROSYNC_SOURCES}
        ${FKX_PUB_HEADER}
)

add_dependencies(aerosync.krnl fkx_key_header)

# ----------------------------------------------------------------------------
# base
# ----------------------------------------------------------------------------
target_compile_options(aerosync.krnl PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -m64
        -target ${CLANG_TARGET_TRIPLE}
        -ffreestanding
        -nostdlib
        -fno-builtin
        -mcmodel=kernel
        -mno-red-zone

        -msoft-float
        -mno-implicit-float
        -mno-mmx
        -mno-sse
        -mno-sse2
        -mno-3dnow
        -mno-avx
        -mno-80387

        -fno-pic
        -fno-pie
        -fno-plt
        -fvisibility=hidden
        -fdata-sections
        -ffunction-sections

        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    >
    $<$<COMPILE_LANGUAGE:ASM_NA`SM>:
        -felf64
    >
)

# ----------------------------------------------------------------------------
# optimizations
# ----------------------------------------------------------------------------
target_compile_options(aerosync.krnl PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -O${OPT_LEVEL}
        -g${DSYM_LEVEL}

        -fno-strict-aliasing
        -fno-common
        -fwrapv

        -Werror=implicit-function-declaration
        -Werror=incompatible-pointer-types
        -Werror=return-type
        -Wvla
        -Wshadow
    >
)

# ----------------------------------------------------------------------------
# hardening
# ----------------------------------------------------------------------------

# Stack Protection
if(STACK_PROTECTION_ALL)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fstack-protector-all
            -ftrivial-auto-var-init=pattern
        >
    )
elseif(STACK_PROTECTION)
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
            -mshstk
        >
    )
endif()

# ----------------------------------------------------------------------------
# extra features
# ----------------------------------------------------------------------------

if (LTO)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -flto=full
            -fvirtual-function-elimination
            -fwhole-program-vtables
        >
    )

    target_link_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -flto=full
            -Wl,--lto-O3
        >
    )
endif()

if(CFI_ENABLE)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fsanitize=cfi
            -fvisibility=hidden
        >
    )
endif()

# ----------------------------------------------------------------------------
# sanitizers
# ----------------------------------------------------------------------------
if(SANITIZER)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fsanitize=bounds,shift,integer-divide-by-zero,vla-bound,null,object-size,return
        >
    )
endif()

# ----------------------------------------------------------------------------
# linking
# ----------------------------------------------------------------------------
set(AEROSYNC_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/aerosync.ld" CACHE STRING "AeroSync linker script")

set_target_properties(aerosync.krnl PROPERTIES LINK_DEPENDS "${AEROSYNC_LINKER_SCRIPT}")

target_link_options(aerosync.krnl PRIVATE
    -fuse-ld=lld
    -T ${AEROSYNC_LINKER_SCRIPT}
    -nostdlib
    -static
    -Wl,-melf_x86_64
    -Wl,--gc-sections
    -Wl,--icf=all
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,-z,separate-code
    -Wl,--build-id=sha1
)

if (STRIP)
    add_custom_command(TARGET aerosync.krnl POST_BUILD
        COMMAND ${LLVM_STRIP} --strip-debug --strip-unneeded $<TARGET_FILE:aerosync.krnl>
        COMMENT "Stripping kernel image"
    )
endif()