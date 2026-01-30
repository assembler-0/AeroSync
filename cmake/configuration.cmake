# ============================================================================
# Build Configuration
# ============================================================================
option(STACK_PROTECTION "Enable stack protection" OFF)
option(STACK_PROTECTION_ALL "Enable stack protection (all)" ON)
option(SANITIZER "Enable sanitizers" ON)
option(LTO "Enable link time optimization" ON)

option(INTEL_CET "Enable CET" ON)
option(MOD_STACK_PROTECTION "Enable stack protection for modules" OFF)
option(MOD_SANITIZER "Enable sanitizers for modules" OFF)
option(MOD_LTO "Enable link time optimization for modules" OFF)
option(MOD_INTEL_CET "Enable CET for modules" ON)
