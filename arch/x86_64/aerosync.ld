/* SPDX-License-Identifier: GPL-2.0-only
 *
 * AeroSync monolithic kernel
 *
 * @file aerosync.ld
 * @brief AeroSync kernel linker script
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

ENTRY(start_kernel)

/* Define constants */
PAGE_SIZE = 4K;
KERNEL_BASE = 0xFFFFFFFF80000000;
BYTE = 8;

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
    _kernel_vma_start = .;

    .limine ALIGN(PAGE_SIZE) :
    {
        _limine_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
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
        
        . = ALIGN(8);
        _fkx_ksymtab_start = .;
        KEEP(*(fkx_ksymtab))
        _fkx_ksymtab_end = .;

        . = ALIGN(4);
        __start___ex_table = .;
        KEEP(*(__ex_table))
        __stop___ex_table = .;

        . = ALIGN(PAGE_SIZE);
        _rodata_end = .;
    } :rodata

	.ctors ALIGN(BYTE) :
	{
	    _ctors_start = .;
	    KEEP(*(.ctors*))
	    . = ALIGN(BYTE);
	    _ctors_end = .;
	} :rodata


	.dtors ALIGN(8) :
	{
	    _dtors_start = .;
	    KEEP(*(.dtors*))
   	    . = ALIGN(BYTE);
	    _dtors_end = .;
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

    /*
     * Per-CPU data section (part of data segment).
     * Linked normally at high address.
     * GS_BASE will be adjusted to point to (alloc_ptr - _percpu_start).
     */
    . = ALIGN(PAGE_SIZE);
    _percpu_start = .;
    .percpu : {
        *(.percpu .percpu.*)
        . = ALIGN(PAGE_SIZE);
        _percpu_end = .;
    } :data

    /* Uninitialized data - read-write, zero-initialized */
    .bss (NOLOAD) :
    {
    	. = ALIGN(PAGE_SIZE);
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)         /* Common symbols */
        . = ALIGN(PAGE_SIZE);
        _bss_end = .;
    } :bss

    /* Kernel end marker */
    . = ALIGN(PAGE_SIZE);
    _kernel_vma_end = .;
    _kernel_size = _kernel_vma_end - _kernel_vma_start;

    /DISCARD/ :
    {
    	*(.llvm*)
    	*(.gnu*)
        *(.eh_frame*)
        *(.note*)
        *(.comment*)
        *(.gcc_except_table*)
    }
}

/* Sanity checks */
ASSERT(_kernel_size < 16M, "Kernel too large (>16MB)")
ASSERT(_text_start >= _kernel_vma_start, "Text section misaligned")
ASSERT(_rodata_start >= _text_end, "Rodata section misaligned")
ASSERT(_data_start >= _rodata_end, "Data section misaligned")
ASSERT(_bss_start >= _data_end, "BSS section misaligned")
