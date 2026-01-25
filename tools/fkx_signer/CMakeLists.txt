cmake_minimum_required(VERSION 3.10)
project(fkx_signer C)

set(CMAKE_C_STANDARD 11)

add_executable(fkx_signer
    main.c
    sha512.c
    hmac.c
)

# Use BEFORE to ensure host headers are found first, or just rely on PRIVATE isolation
target_include_directories(fkx_signer PRIVATE .)

target_link_libraries(fkx_signer PRIVATE)