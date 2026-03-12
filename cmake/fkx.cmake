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