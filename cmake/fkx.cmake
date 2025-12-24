function(add_fkx_module MODULE_NAME)
    # Build as a shared library (or static, depending on format)
    add_executable(${MODULE_NAME} ${ARGN})

    # Freestanding compilation
    target_compile_options(${MODULE_NAME} PRIVATE
            -ffreestanding
            -fno-builtin
            -fno-exceptions
            -fno-rtti
            -fno-stack-protector
            -fPIC
            -m64
            -mcmodel=kernel
    )

    # Link flags
    target_link_options(${MODULE_NAME} PRIVATE
            -fuse-ld=lld
            -nostdlib
            -shared
            -Wl,-melf_x86_64
    )

    if(LTO)
        target_compile_options(${MODULE_NAME} PRIVATE -flto)
        target_link_options(${MODULE_NAME} PRIVATE -flto)
    endif()

    # Set output extension
    set_target_properties(${MODULE_NAME} PROPERTIES
            SUFFIX ".module.fkx"
    )

    # Sign module after build
    # later please

    # Copy module to boot directory
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bootdir/kernel
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${MODULE_NAME}>
            ${CMAKE_BINARY_DIR}/bootdir/module/$<TARGET_FILE_NAME:${MODULE_NAME}>
            COMMENT "Copying FKX module ${MODULE_NAME} to bootdir"
    )

    # Register module for ISO dependency
    set_property(GLOBAL APPEND PROPERTY FKX_MODULES ${MODULE_NAME})
endfunction()