set(PCI_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/pci)

set(PCI_SOURCES
        ${PCI_SOURCE_DIR}/pci.c
)

add_fkx_module(pci ${PCI_SOURCES})
