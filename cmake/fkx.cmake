# FKX Module Build System

# Path to the signing key
set(FKX_KEY_DIR "${CMAKE_SOURCE_DIR}/keys")
set(FKX_KEY_FILE "${FKX_KEY_DIR}/fkx_root.key")
set(FKX_PUB_HEADER "${CMAKE_BINARY_DIR}/fkx_key.h")

# Ensure keys directory exists at configure time
file(MAKE_DIRECTORY ${FKX_KEY_DIR})

# Add a rule to generate the key file if it doesn't exist
add_custom_command(
    OUTPUT ${FKX_KEY_FILE}
    COMMAND $<TARGET_FILE:fkx_signer> genkey ${FKX_KEY_FILE}
    DEPENDS fkx_signer
    COMMENT "Generating new FKX signing key..."
)

# Create a C header from the key for the kernel to include
add_custom_command(
    OUTPUT ${FKX_PUB_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT_FILE=${FKX_KEY_FILE} -DOUTPUT_FILE=${FKX_PUB_HEADER} -P ${CMAKE_SOURCE_DIR}/cmake/generate_key_header.cmake
    DEPENDS ${FKX_KEY_FILE} ${CMAKE_SOURCE_DIR}/cmake/generate_key_header.cmake
    COMMENT "Generating FKX key header for kernel"
)

# Target for the key header
add_custom_target(fkx_key_header DEPENDS ${FKX_PUB_HEADER})

function(add_fkx_module MODULE_NAME)
    # Build as a shared library (or static, depending on format)
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
            -target ${CLANG_TARGET_TRIPLE}
   	        -O${OPT_LEVEL}
   	        -g${DSYM_LEVEL}
   	        -fdata-sections
   	        -ffunction-sections
   	        -fno-omit-frame-pointer
   	        -finline-functions
   	        -foptimize-sibling-calls
   	        -ffreestanding
   	        -mno-implicit-float
   	        -msoft-float
   	        -mno-red-zone
   	        -mserialize
   	        -fPIC
   	        -mcmodel=kernel
   	        -fcf-protection=full
   	        -fvisibility=hidden
   	    >
   	    $<$<COMPILE_LANGUAGE:ASM_NASM>:
   	        -felf64
   	    >
   	)

    # Link flags
    target_link_options(${MODULE_NAME} PRIVATE
            -fuse-ld=lld
            -nostdlib
            -shared
            -Wl,-melf_x86_64
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
            SUFFIX ".module.fkx"
    )

    # Sign module after build
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND $<TARGET_FILE:fkx_signer> sign $<TARGET_FILE:${MODULE_NAME}> ${FKX_KEY_FILE}
            COMMENT "Signing FKX module ${MODULE_NAME}"
    )

    # Copy module to boot directory
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bootdir/module
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${MODULE_NAME}>
            ${CMAKE_BINARY_DIR}/bootdir/module/$<TARGET_FILE_NAME:${MODULE_NAME}>
            COMMENT "Copying signed FKX module ${MODULE_NAME} to bootdir"
    )

    # Register module for ISO dependency
    set_property(GLOBAL APPEND PROPERTY FKX_MODULES ${MODULE_NAME})
endfunction()