/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/scatterlist.h
 * @brief Scatter-gather list support for DMA
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/sysintf/dma.h>

/**
 * struct scatterlist - Scatter-gather list entry
 * @dma_address: DMA address of this segment
 * @length: Length of this segment in bytes
 * @offset: Offset into the page
 * @page_link: Encoded page pointer (low bits used for flags)
 */
struct scatterlist {
  uint64_t page_link;
  uint32_t offset;
  uint32_t length;
  dma_addr_t dma_address;
  uint32_t dma_length;
};

#define SG_CHAIN 0x01UL
#define SG_END   0x02UL

/**
 * sg_init_table - Initialize scatter-gather table
 * @sgl: Scatter-gather list
 * @nents: Number of entries
 */
void sg_init_table(struct scatterlist *sgl, unsigned int nents);

/**
 * sg_set_page - Set page for scatter-gather entry
 * @sg: Scatter-gather entry
 * @page: Page pointer
 * @len: Length in bytes
 * @offset: Offset into page
 */
void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset);

/**
 * sg_set_buf - Set buffer for scatter-gather entry
 * @sg: Scatter-gather entry
 * @buf: Virtual address
 * @buflen: Length in bytes
 */
void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen);

/**
 * sg_next - Get next scatter-gather entry
 * @sg: Current entry
 *
 * Returns: Next entry or nullptr if at end
 */
struct scatterlist *sg_next(struct scatterlist *sg);

/**
 * sg_dma_address - Get DMA address of scatter-gather entry
 * @sg: Scatter-gather entry
 *
 * Returns: DMA address
 */
static inline dma_addr_t sg_dma_address(struct scatterlist *sg) {
  return sg->dma_address;
}

/**
 * sg_dma_len - Get DMA length of scatter-gather entry
 * @sg: Scatter-gather entry
 *
 * Returns: DMA length in bytes
 */
static inline uint32_t sg_dma_len(struct scatterlist *sg) {
  return sg->dma_length;
}

/**
 * for_each_sg - Iterate over scatter-gather list
 * @sglist: Scatter-gather list
 * @sg: Iterator variable
 * @nr: Number of entries
 * @__i: Loop counter
 */
#define for_each_sg(sglist, sg, nr, __i) \
  for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))

/**
 * dma_map_sg - Map scatter-gather list for DMA
 * @dev: Device (currently unused, for future IOMMU support)
 * @sg: Scatter-gather list
 * @nents: Number of entries
 * @dir: DMA direction
 *
 * Maps a scatter-gather list for DMA access. May coalesce adjacent
 * segments if CONFIG_DMA_SG_COALESCING is enabled.
 *
 * Returns: Number of DMA segments (may be less than nents due to coalescing)
 */
int dma_map_sg(void *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);

/**
 * dma_unmap_sg - Unmap scatter-gather list
 * @dev: Device
 * @sg: Scatter-gather list
 * @nents: Number of entries returned by dma_map_sg()
 * @dir: DMA direction
 */
void dma_unmap_sg(void *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);

/**
 * dma_sync_sg_for_cpu - Sync scatter-gather list for CPU access
 * @dev: Device
 * @sg: Scatter-gather list
 * @nents: Number of entries
 * @dir: DMA direction
 *
 * Ensures CPU can safely read data written by device.
 */
void dma_sync_sg_for_cpu(void *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);

/**
 * dma_sync_sg_for_device - Sync scatter-gather list for device access
 * @dev: Device
 * @sg: Scatter-gather list
 * @nents: Number of entries
 * @dir: DMA direction
 *
 * Ensures device can safely read data written by CPU.
 */
void dma_sync_sg_for_device(void *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);
