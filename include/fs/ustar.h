#ifndef USTAR_H
#define USTAR_H

#include <kernel/types.h>

// USTAR (tar) header structure (512 bytes)
struct ustar_header {
    char name[100];     // File name
    char mode[8];       // File mode (octal)
    char uid[8];        // Owner's user ID (octal)
    char gid[8];        // Owner's group ID (octal)
    char size[12];      // File size in bytes (octal)
    char mtime[12];     // Last modification time (octal, Unix epoch)
    char chksum[8];     // Checksum of header (octal)
    char typeflag;      // Type of file (see below)
    char linkname[100]; // Name of linked file
    char magic[6];      // USTAR magic string "ustar"
    char version[2];    // USTAR version "00"
    char uname[32];     // Owner user name (ASCII)
    char gname[32];     // Owner group name (ASCII)
    char devmajor[8];   // Device major number (octal)
    char devminor[8];   // Device minor number (octal)
    char prefix[155];   // Prefix for file name (for long names)
    char padding[12];   // Padding to 512 bytes (total 512 bytes)
};

// Typeflag values
#define USTAR_FILE        '0' // Regular file
#define USTAR_LINK        '1' // Hard link
#define USTAR_SYMLINK     '2' // Symbolic link
#define USTAR_CHARDEV     '3' // Character device
#define USTAR_BLOCKDEV    '4' // Block device
#define USTAR_DIRECTORY   '5' // Directory
#define USTAR_FIFO        '6' // FIFO (named pipe)
#define USTAR_CONTIGUOUS  '7' // Contiguous file (archaic)

// USTAR magic and version
#define USTAR_MAGIC "ustar" // "ustar\0" (6 bytes including null terminator)
#define USTAR_VERSION "00"  // "00"

// Functions exposed by ustar.c
uint64_t ustar_oct_to_bin(const char *octal, size_t size);
unsigned int ustar_checksum(const struct ustar_header *header);

#endif // USTAR_H
