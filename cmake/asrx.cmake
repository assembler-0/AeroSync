# ASRX Module Build System

# Path to the signing key (Reuses FKX root key)
set(FKX_KEY_DIR "${CMAKE_SOURCE_DIR}/keys")
set(FKX_KEY_FILE "${FKX_KEY_DIR}/fkx_root.key")

function(add_asrx_module MODULE_NAME)
    # Build as a loadable module
    add_executable(${MODULE_NAME} ${ARGN})

    # Ensure the signer and key header are ready before the module
    add_dependencies(${MODULE_NAME} fkx_signer fkx_key_header)

    target_include_directories(${MODULE_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/lib
        ${CMAKE_BINARY_DIR}
    )

    # Freestanding compilation
   	target_compile_options(${MODULE_NAME} PRIVATE
   	    $<$<COMPILE_LANGUAGE:C>:
   	        -m64
   	        -O${OPT_LEVEL}
   	        -g${DSYM_LEVEL}
   	        -fdata-sections
   	        -ffunction-sections
   	        -fno-omit-frame-pointer
   	        -finline-functions
   	        -foptimize-sibling-calls
   	        -ffreestanding
   	        -msoft-float
   	        -mno-red-zone
   	        -mserialize
   	        -fPIC
   	        -fcf-protection=full
   	        -fvisibility=hidden
   	    >
   	    $<$<COMPILE_LANGUAGE:ASM_NASM>:
   	        -felf64
   	    >
   	)

    if(COMPILER_IDENTIFIER STREQUAL "clang")
        target_compile_options(${MODULE_NAME} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:
                -target ${CLANG_TARGET_TRIPLE}
                -mno-implicit-float
                -mcmodel=kernel
            >
        )
    endif()

    # Link flags
    target_link_options(${MODULE_NAME} PRIVATE
            -fuse-ld=lld
            -nostdlib
            -shared
            -Wl,-melf_x86_64
            -Wl,--unresolved-symbols=ignore-all
    )

	if(MOD_SANITIZER)
	    target_compile_options(${MODULE_NAME} PRIVATE
	        $<$<COMPILE_LANGUAGE:C>:
	            -fsanitize=undefined,bounds,null,return,vla-bound
	        >
	    )
	endif()

	if(MOD_STACK_PROTECTION)
	    target_compile_options(${MODULE_NAME} PRIVATE
	        $<$<COMPILE_LANGUAGE:C>:
	            -fstack-protector-all
	            -D_FORTIFY_SOURCE=2
	        >
	    )
	endif()

	if(MOD_INTEL_CET)
		target_compile_options(${MODULE_NAME} PRIVATE
			$<$<COMPILE_LANGUAGE:C>:
				-fcf-protection=full
			>
		)
	endif()

    # Set output extension
    set_target_properties(${MODULE_NAME} PROPERTIES
            SUFFIX ".module.asrx"
    )

    # Sign module after build
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND $<TARGET_FILE:fkx_signer> sign $<TARGET_FILE:${MODULE_NAME}> ${FKX_KEY_FILE}
            COMMENT "Signing ASRX module ${MODULE_NAME}"
    )

    # Copy module to boot directory
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bootdir/module
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${MODULE_NAME}>
            ${CMAKE_BINARY_DIR}/bootdir/module/$<TARGET_FILE_NAME:${MODULE_NAME}>
            COMMENT "Copying signed ASRX module ${MODULE_NAME} to bootdir"
    )

    # Register module for ISO dependency
    set_property(GLOBAL APPEND PROPERTY ASRX_MODULES ${MODULE_NAME})
endfunction()
