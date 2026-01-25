/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/ustar.c
 * @brief USTAR archive parser
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

#include <fs/ustar.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/types.h>

// Helper function to convert octal string to binary
uint64_t ustar_oct_to_bin(const char *octal, size_t size) {
    uint64_t bin = 0;
    size_t i = 0;
    // Iterate through the string, stopping at null terminator or non-octal digit
    while (i < size && octal[i] != '\0' && octal[i] >= '0' && octal[i] <= '7') {
        bin = (bin * 8) + (octal[i] - '0');
        i++;
    }
    return bin;
}

// Helper function to calculate the checksum of a USTAR header
unsigned int ustar_checksum(const struct ustar_header *header) {
    unsigned int sum = 0;
    const unsigned char *ptr = (const unsigned char *)header;

    // Sum all bytes in the header, treating the checksum field as spaces
    for (size_t i = 0; i < sizeof(struct ustar_header); i++) {
        if (i >= offsetof(struct ustar_header, chksum) &&
            i < offsetof(struct ustar_header, chksum) + sizeof(header->chksum)) {
            sum += ' ';
        } else {
            sum += ptr[i];
        }
    }
    return sum;
}

// Function to parse the USTAR archive
// This function currently just prints file information.
// It will be extended later to populate a filesystem.
void ustar_parse_archive(const void *archive_start, size_t archive_size) {
    const uint8_t *current_ptr = (const uint8_t *)archive_start;
    const uint8_t *archive_end = (const uint8_t *)archive_start + archive_size;

    printk(USTAR_CLASS "Parsing archive at %p, size %lu\n", archive_start, archive_size);

    while (current_ptr < archive_end) {
        const struct ustar_header *header = (const struct ustar_header *)current_ptr;

        // Check for end of archive (two consecutive 512-byte zero blocks)
        // A robust check involves looking at two blocks.
        // For simplicity, we check if the current block is all zeros.
        bool current_block_all_zero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (((const uint8_t*)header)[i] != 0) {
                current_block_all_zero = false;
                break;
            }
        }

        if (current_block_all_zero) {
            // If the current block is all zeros, it might be the end.
            // Check if there's enough space for another zero block.
            if (current_ptr + 512 <= archive_end) {
                const uint8_t *next_block_ptr = current_ptr + 512;
                bool next_block_all_zero = true;
                for (size_t i = 0; i < 512; ++i) {
                    if (next_block_ptr[i] != 0) {
                        next_block_all_zero = false;
                        break;
                    }
                }
                if (next_block_all_zero) {
                    printk(USTAR_CLASS "End of archive (two zero blocks) detected at %p.\n", current_ptr);
                    break;
                }
            } else {
                // Not enough space for another zero block, but current is zero.
                // Assume end of archive if this is the very last block.
                printk(USTAR_CLASS "End of archive (single trailing zero block) detected at %p.\n", current_ptr);
                break;
            }
        }
        

        // Basic header validation: magic and version
        // USTAR_MAGIC is "ustar\0", so strncmp for 5 chars is correct to ignore the null.
        // USTAR_VERSION is "00", strncmp for 2 chars.
        if (strncmp(header->magic, USTAR_MAGIC, 5) != 0 ||
            strncmp(header->version, USTAR_VERSION, 2) != 0) {
            printk(USTAR_CLASS "Invalid USTAR header magic ('%.5s' vs 'ustar') or version ('%.2s' vs '00') at %p. Stopping parsing.\n",
                   header->magic, header->version, current_ptr);
            break; // Stop parsing on first invalid header
        }

        // Validate checksum
        unsigned int expected_checksum = ustar_oct_to_bin(header->chksum, sizeof(header->chksum));
        unsigned int actual_checksum = ustar_checksum(header);

        if (expected_checksum != actual_checksum) {
            printk(USTAR_CLASS "Checksum mismatch for file '%.100s'. Expected %u, got %u. Skipping file.\n",
                   header->name, expected_checksum, actual_checksum);
            // Advance to next header block, skipping current file data
            uint64_t file_size = ustar_oct_to_bin(header->size, sizeof(header->size));
            uint64_t data_blocks = (file_size + 511) / 512;
            current_ptr += (1 + data_blocks) * 512; // 1 for header, data_blocks for data
            
            // Ensure we don't go past the end of the archive
            if (current_ptr > archive_end) {
                printk(USTAR_CLASS "Archive truncated or malformed, skipping past end of archive.\n");
                break;
            }
            continue;
        }

        uint64_t file_size = ustar_oct_to_bin(header->size, sizeof(header->size));
        // Calculate the address of the data start
        const void *file_data_start = (const void *)(current_ptr + 512);

        printk(USTAR_CLASS "File: '%.100s', type: %c, size: %lu, data_addr: %p\n",
               header->name, header->typeflag, file_size, file_data_start);
        
        // Advance current_ptr to the next header
        // Header block + data blocks (rounded up to nearest 512-byte block)
        uint64_t total_blocks_for_entry = 1 + (file_size + 511) / 512;
        current_ptr += total_blocks_for_entry * 512;

        if (current_ptr > archive_end) {
            printk(USTAR_CLASS "Archive truncated or malformed, file '%.100s' extends beyond archive size.\n", header->name);
            break;
        }
    }
    printk(USTAR_CLASS "Archive parsing finished.\n");
}
