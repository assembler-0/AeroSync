set(PCI_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/pci)

file(GLOB_RECURSE PCI_SOURCES "${PCI_SOURCE_DIR}/*.c")

add_fkx_module(pci ${PCI_SOURCES})
