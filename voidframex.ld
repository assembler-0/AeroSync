/*
* VoidFrameX kernel linker script optimized for x86_64
*/

ENTRY(start_kernel)

/* Define constants */
PAGE_SIZE = 4K;
KERNEL_BASE = 0xFFFFFFFF80000000;

PHDRS
{
    requests PT_LOAD    FLAGS(6); /* R(4) | W(2) = 6 */
    text     PT_LOAD    FLAGS(5); /* R(4) | E(1) = 5 */
    rodata   PT_LOAD    FLAGS(4); /* R(4) */
    data     PT_LOAD    FLAGS(6); /* R(4) | W(2) = 6 */
    bss      PT_LOAD    FLAGS(6); /* R(4) | W(2) = 6 */
}

SECTIONS
{
    . = KERNEL_BASE;
    _kernel_phys_start = .;

    .limine ALIGN(PAGE_SIZE) :
    {
        _limine_start = .;
        KEEP(*(.limine_header))
        KEEP(*(.limine_requests))
        . = ALIGN(PAGE_SIZE);
        _limine_end = .;
    } :requests

    .init ALIGN(PAGE_SIZE) :
    {
        _init_start = .;
        *(.init.text)
        *(.init.data)
        . = ALIGN(PAGE_SIZE);
        _init_end = .;
    } :text

    /* Executable code - read-only, executable */
    .text ALIGN(PAGE_SIZE) :
    {
        _text_start = .;
        *(.text.hot)      /* Hot/frequently used code */
        *(.text)          /* Regular code */
        *(.text.*)        /* Other text sections */
        . = ALIGN(PAGE_SIZE);
        _text_end = .;
    } :text

    /* Read-only data - read-only, non-executable */
    .rodata ALIGN(PAGE_SIZE) :
    {
        _rodata_start = .;
        *(.rodata.str*)   /* String literals first */
        *(.rodata)        /* Regular rodata */
        *(.rodata.*)
        . = ALIGN(PAGE_SIZE);
        _rodata_end = .;
    } :rodata

    /* Initialized data - read-write */
    .data ALIGN(PAGE_SIZE) :
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(PAGE_SIZE);
        _data_end = .;
    } :data

    /* Uninitialized data - read-write, zero-initialized */
    .bss ALIGN(PAGE_SIZE) :
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)         /* Common symbols */
        . = ALIGN(PAGE_SIZE);
        _bss_end = .;
    } :bss

    /* Kernel end marker */
    . = ALIGN(PAGE_SIZE);
    _kernel_phys_end = .;
    _kernel_size = _kernel_phys_end - _kernel_phys_start;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note*)
        *(.comment)
    }
}

/* Sanity checks */
ASSERT(_kernel_size < 16M, "Kernel too large (>16MB)")
ASSERT(_text_start >= _kernel_phys_start, "Text section misaligned")
ASSERT(_rodata_start >= _text_end, "Rodata section misaligned")
ASSERT(_data_start >= _rodata_end, "Data section misaligned")
ASSERT(_bss_start >= _data_end, "BSS section misaligned")
