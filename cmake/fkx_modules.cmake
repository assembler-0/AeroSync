# bunch of if-else statement to decide if a module should be built

function(include_ifdef config path)
    if(DEFINED ${config})
        include(${CMAKE_SOURCE_DIR}/${path})
    endif()
endfunction()

include_ifdef(CONFIG_EXT_DRM_RENDERER drivers/graphics/drm/linearfb/linearfb.cmake)
include_ifdef(CONFIG_EXT_UART drivers/uart/uart.cmake)
include_ifdef(CONFIG_EXT_INTEL_IOMMU drivers/iommu/vtd.cmake)
include_ifdef(CONFIG_EXT_IDE_ATA drivers/block/ide/ide.cmake)
include_ifdef(CONFIG_EXT_RT_FW_SUPPORT drivers/fw/fw.cmake)

include(drivers/apic/ic.cmake)
include(drivers/pci/pci.cmake)
include(drivers/timer/timer.cmake)
include(drivers/char/pty.cmake)
