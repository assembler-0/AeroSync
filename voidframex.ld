/*
* VoidFrameX kernel linker script optimized for x86_64
*/

ENTRY(start_kernel)

/* Define constants */
PAGE_SIZE = 4K;
KERNEL_BASE = 0;

SECTIONS
{
    . = KERNEL_BASE;
    _kernel_phys_start = .;

    .init ALIGN(PAGE_SIZE) :
    {
        _init_start = .;
        *(.init.text)
        *(.init.data)
        . = ALIGN(PAGE_SIZE);
        _init_end = .;
    }

    /* Executable code - read-only, executable */
    .text ALIGN(PAGE_SIZE) :
    {
        _text_start = .;
        *(.text.boot)     /* Boot code first */
        *(.text.hot)      /* Hot/frequently used code */
        *(.text)          /* Regular code */
        *(.text.*)        /* Other text sections */
        . = ALIGN(PAGE_SIZE);
        _text_end = .;
    }

    /* Read-only data - read-only, non-executable */
    .rodata ALIGN(PAGE_SIZE) :
    {
        _rodata_start = .;
        *(.rodata.str*)   /* String literals first */
        *(.rodata)        /* Regular rodata */
        *(.rodata.*)
        . = ALIGN(PAGE_SIZE);
        _rodata_end = .;
    }

    /* Initialized data - read-write */
    .data ALIGN(PAGE_SIZE) :
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(PAGE_SIZE);
        _data_end = .;
    }

    /* Uninitialized data - read-write, zero-initialized */
    .bss ALIGN(PAGE_SIZE) :
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)         /* Common symbols */
        . = ALIGN(PAGE_SIZE);
        _bss_end = .;
    }

    /* Kernel end marker */
    . = ALIGN(PAGE_SIZE);
    _kernel_phys_end = .;
    _kernel_size = _kernel_phys_end - _kernel_phys_start;
}

/* Sanity checks */
ASSERT(_kernel_size < 16M, "Kernel too large (>16MB)")
ASSERT(_text_start >= _kernel_phys_start, "Text section misaligned")
ASSERT(_rodata_start >= _text_end, "Rodata section misaligned")
ASSERT(_data_start >= _rodata_end, "Data section misaligned")
ASSERT(_bss_start >= _data_end, "BSS section misaligned")
