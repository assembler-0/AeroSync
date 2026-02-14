set(VTD_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/iommu/intel)

file(GLOB_RECURSE VTD_SOURCES "${VTD_SOURCE_DIR}/*.c")

add_fkx_module(vtd ${VTD_SOURCES})
