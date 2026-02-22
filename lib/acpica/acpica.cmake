set(ACPICA_DIR ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE ACPICA_SOURCES
    ${ACPICA_DIR}/source/*.c
)

set(ACPICA_INCLUDES
    ${ACPICA_DIR}/source/include
)

# please add these definitions when compiling
# target_compile_definitions(acpica
#         ACPI_DEBUG_OUTPUT=1
#         ACPI_LIBRARY
#         __aerosync__
# )
