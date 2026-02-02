set(LINEARFB_SOURCE_DIR ${CMAKE_SOURCE_DIR}/drivers/graphics/drm/linearfb)

set(LINEARFB_SOURCES
    ${LINEARFB_SOURCE_DIR}/linearfb.c
    ${LINEARFB_SOURCE_DIR}/psf.c
    ${LINEARFB_SOURCE_DIR}/embedded_font.c
)

add_fkx_module(linearfb ${LINEARFB_SOURCES})