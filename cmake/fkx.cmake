function(add_fkx_module MODULE_NAME)
    # Build as a shared library (or static, depending on format)
    add_executable(${MODULE_NAME} ${ARGN})

    # Freestanding compilation
   	target_compile_options(${MODULE_NAME} PRIVATE
   	    $<$<COMPILE_LANGUAGE:C>:
   	        -m64
   	        -target ${CLANG_TARGET_TRIPLE}
   	        -O2
   	        -g0
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

    if(MOD_LTO)
        target_compile_options(${MODULE_NAME} PRIVATE 
        	$<$<COMPILE_LANGUAGE:C>:
        		-flto
        	>
       	)
        target_link_options(${MODULE_NAME} PRIVATE 
        	-flto
        	-Wl,--gc-sections
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
