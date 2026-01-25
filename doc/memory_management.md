# AeroSync Memory Management System

## Overview

AeroSync implements a sophisticated memory management system that handles both physical and virtual memory management. The system is designed with modern concepts like NUMA awareness, advanced page replacement algorithms, and efficient allocation strategies.

## Physical Memory Management (PMM)

### Core Concepts

The Physical Memory Manager (PMM) manages the system's physical RAM using several key abstractions:

- **struct page**: Represents a single 4KB physical page frame with metadata flags
- **struct folio**: Represents a contiguous set of pages managed as a unit
- **struct zone**: Logical grouping of pages (DMA, DMA32, NORMAL)
- **struct pglist_data**: NUMA node data containing zones and statistics

### Page Management

Pages are tracked using bit flags for state management:
- `PG_reserved`: Reserved for kernel use
- `PG_buddy`: Part of the buddy allocator
- `PG_active`: On active LRU list
- `PG_slab`: Used by SLUB allocator
- `PG_referenced`: Recently accessed
- `PG_lru`: On LRU list
- `PG_head`/`PG_tail`: For compound pages (folios)

### Buddy Allocator

The system uses a classic buddy allocator for managing physically contiguous page blocks:
- Supports orders 0 to MAX_ORDER (typically up to 11, for 4MB blocks)
- Maintains free lists for each order and migration type
- Implements fragmentation control through migration types

### Per-CPU Page Caching

To reduce lock contention, each CPU maintains local page caches:
- Hot, cold, and leftover page lists
- Reduces global lock acquisition for frequent allocations
- Improves NUMA locality

### NUMA Awareness

The PMM is fully NUMA-aware:
- Each node has its own `pglist_data` structure
- Zones are organized per-node
- Allocation policies consider NUMA topology
- Cross-node fallback mechanisms exist

## Virtual Memory Management (VMM)

### Address Space Representation

Each process has an `mm_struct` representing its virtual address space:
- Contains a maple tree for VMA management
- Tracks page table root (PML4/5)
- Maintains accounting information
- Includes memory region hints and boundaries

### Virtual Memory Areas (VMAs)

VMAs (`vm_area_struct`) represent contiguous regions of virtual memory:
- Defined by start/end addresses and protection flags
- Support different memory types (anonymous, file-backed, shared)
- Include operation callbacks for fault handling
- Managed in a maple tree for efficient lookup

### Virtual Memory Allocation (vmalloc)

The vmalloc subsystem provides high-performance virtual address allocation:
- Hybrid approach combining Linux, BSD, and XNU techniques
- Uses augmented RB-trees for O(log N) gap searches
- Implements per-CPU caching to reduce lock contention
- Sub-allocates within larger blocks (vmap_blocks)
- Supports NUMA-partitioned address spaces
- Features lazy purging for improved performance

### Page Fault Handling

The system handles page faults efficiently:
- Hardware page faults trigger fault handlers
- COW (Copy-on-Write) for shared pages
- Demand paging for file-backed mappings
- Swap/compression support for anonymous pages

## Kernel Memory Allocation

### SLUB Allocator

AeroSync uses a sophisticated SLUB (Unqueued Slab) allocator:
- Lockless per-CPU caches for hot objects
- Magazine layer (BSD/XNU hybrid) for bulk operations
- Implements transaction IDs for lockless operations
- Supports NUMA-aware allocation
- Includes poisoning and redzone features for debugging
- Provides bulk allocation APIs for performance

### Allocation APIs

- `kmalloc()`/`kfree()`: General-purpose kernel allocation
- `kzalloc()`: Zero-initialized allocation
- `krealloc()`: Resizing allocations
- `kmem_cache_create()`: Custom cache creation
- `slab_sheaf`: Bulk allocation API for performance-critical paths

## Advanced Memory Features

### Zone Management (ZMM)

The Zone Memory Manager provides memory compression:
- Compresses cold anonymous pages to reclaim memory
- Maintains handles to compressed data
- Transparent decompression when pages are accessed
- Configurable via `CONFIG_MM_ZMM`

### Shared Memory (SHM) (stub!)

Named shared memory objects allow inter-process communication:
- Reference-counted objects with automatic cleanup
- Named access for easy sharing
- Integration with VMO (Virtual Memory Objects) system
- Automatic cleanup when references reach zero

### Memory Compaction

The system includes memory compaction to reduce fragmentation:
- Migrates pages to create larger contiguous blocks
- Works with the LRU system to identify movable pages
- Runs periodically or on-demand

### Memory Reclaim

Advanced memory reclaim mechanisms:
- kswapd: Background page reclaimer
- khugepaged: Transparent huge page management
- Writeback mechanisms for dirty pages
- LRU generation-based page replacement (MGLRU)

## Memory Protection and Hardening

### Stack Protection

- SSP (Stack Smashing Protection) implementation
- Canary values to detect buffer overflows
- Runtime integrity checking

### Memory Sanitization

- Optional memory sanitization features in `mm/san/`
- Tools for detecting use-after-free and out-of-bounds access
- Compile-time configurable

## Key Data Structures

### Memory Zones
- `ZONE_DMA`: For legacy ISA devices
- `ZONE_DMA32`: For 32-bit DMA-capable devices
- `ZONE_NORMAL`: General system memory

### Page Replacement
- Multi-generation LRU (MGLRU) for efficient page selection
- Separate lists for file and anonymous pages
- Generation-based aging system

### Memory Statistics
- Per-zone and per-node accounting
- Atomic counters for performance
- Integration with system monitoring

## Configuration Options

The memory management system supports various compile-time options:
- `CONFIG_MM_ZMM`: Memory compression
- `CONFIG_SLAB_MAX_ORDER`: Maximum slab allocation size
- `CONFIG_SLAB_MAG_SIZE`: Magazine layer size
- `MM_HARDENING`: Memory hardening features
- `CONFIG_MAX_NUMNODES`: Maximum NUMA nodes
> see `mm/Kconfig` for a complete list of available options
 
For implementation details, refer to the source files in `mm/` and `arch/x86_64/mm/` directories.