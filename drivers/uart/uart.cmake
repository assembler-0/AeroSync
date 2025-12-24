set(UART_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/uart)

set(UART_SOURCES
        ${UART_SOURCE_DIR}/serial.c
)

add_fkx_module(uart ${UART_SOURCES})