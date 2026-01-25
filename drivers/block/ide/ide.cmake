set(IDE_SOURCES
    drivers/block/ide/ide.c
    drivers/block/ide/ide_pio.c
    drivers/block/ide/ide_dma.c
)

add_fkx_module(ide ${IDE_SOURCES})
