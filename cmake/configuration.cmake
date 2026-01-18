# ============================================================================
# Build Configuration
# ============================================================================
option(STACK_PROTECTION "Enable stack protection" ON)
option(SANITIZER "Enable sanitizers" ON)
option(MM_HARDENING "Enable MM hardening (poisoning, redzones, etc)" ON)
option(LTO "Enable link time optimization" ON)
option(INTEL_CET "Enable CET" ON)
option(MOD_STACK_PROTECTION "Enable stack protection for modules" OFF)
option(MOD_SANITIZER "Enable sanitizers for modules" OFF)
option(MOD_LTO "Enable link time optimization for modules" OFF)
option(MOD_INTEL_CET "Enable CET for modules" ON)
