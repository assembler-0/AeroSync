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
            ENABLE_EXPORTS ON
    )

    # Sign module after build
#    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
#            COMMAND ${CMAKE_SOURCE_DIR}/tools/sign_fkx $<TARGET_FILE:${MODULE_NAME}> # later
#            COMMENT "Signing FKX module: ${MODULE_NAME}"
#    )
endfunction()