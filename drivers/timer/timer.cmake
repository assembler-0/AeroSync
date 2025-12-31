set(TIMER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/timer)

file(GLOB_RECURSE TIMER_SOURCES "${TIMER_SOURCE_DIR}/*.c")

add_fkx_module(timer ${TIMER_SOURCES})