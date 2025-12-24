set(LINEARFB_SOURCE_DIR ${CMAKE_SOURCE_DIR}/lib/linearfb)

set(LINEARFB_SOURCES
    ${LINEARFB_SOURCE_DIR}/font.c
    ${LINEARFB_SOURCE_DIR}/linearfb.c
)

add_fkx_module(linearfb ${LINEARFB_SOURCES})