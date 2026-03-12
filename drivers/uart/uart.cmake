set(UART_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/uart)

set(UART_SOURCES
        ${UART_SOURCE_DIR}/serial_core.c
        ${UART_SOURCE_DIR}/serial.c
)

add_asrx_module(uart ${UART_SOURCES})