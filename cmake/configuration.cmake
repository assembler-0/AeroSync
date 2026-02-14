# ============================================================================
# Build Configuration
# ============================================================================
option(STACK_PROTECTION "Enable stack protection" OFF)
option(STACK_PROTECTION_ALL "Enable stack protection (all)" ON)
option(SANITIZER "Enable sanitizers" ON)
option(LTO "Enable link time optimization" ON)
option(STRIP "Strip symbol after build" OFF)
option(INTEL_CET "Enable CET" ON)
option(CFI_ENABLE "Enable CFI" OFF)

option(MOD_STACK_PROTECTION "Enable stack protection for modules" OFF)
option(MOD_SANITIZER "Enable sanitizers for modules" OFF)
option(MOD_INTEL_CET "Enable CET for modules" ON)

# Optimization & Debug Levels
# Defaults provided here, can be overridden by presets or -D
if(NOT DEFINED OPT_LEVEL)
    set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
endif()

if(NOT DEFINED DSYM_LEVEL)
    set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
endif()

# Initrd/Initramfs
if(NOT DEFINED AEROSYNC_INITRD)
    set(AEROSYNC_INITRD "" CACHE FILEPATH "Path to the initramfs/initrd CPIO archive")
endif()