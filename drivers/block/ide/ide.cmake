set(IDE_SOURCES
    drivers/block/ide/ide.c
    drivers/block/ide/ide_pio.c
    drivers/block/ide/ide_dma.c
)

add_asrx_module(ide ${IDE_SOURCES})
