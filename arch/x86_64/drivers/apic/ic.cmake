set(IC_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/apic)

file(GLOB_RECURSE IC_SOURCES "${IC_SOURCE_DIR}/*.c")

add_fkx_module(ic ${IC_SOURCES})