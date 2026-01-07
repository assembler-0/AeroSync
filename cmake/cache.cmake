# ============================================================================
# Cache variables
# ============================================================================

set(CLANG_TARGET_TRIPLE "x86_64-unknown-none-elf" CACHE STRING "Clang target triple for cross-compilation")
set_property(CACHE CLANG_TARGET_TRIPLE PROPERTY STRINGS x86_64-unknown-none-elf x86_64-pc-none-elf x86_64-pc-linux-gnu) # Why would anyone use linux-gnu for a kernel? Who knows.

set(ALLOWED_C_COMPILER "clang|icx|aocc" CACHE STRING "Allowed C compilers")
set(ALLOWED_CXX_COMPILER "clang++|icpx|aocc" CACHE STRING "Allowed C++ compilers")

