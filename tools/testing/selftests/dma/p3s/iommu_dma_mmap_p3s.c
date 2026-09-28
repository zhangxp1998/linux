// SPDX-License-Identifier: GPL-2.0
/*
 * A noncontiguous IOMMU DMA buffer exported by the iommu_dma_mmap_p3s
 * fixture maps completely into a 4KB process and preserves all four
 * process-page slices.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_harness.h"

#define BUFFER_SIZE (4 * PROCESS_PAGE_SIZE)

TEST(noncontiguous_dma_buffer_maps_all_four_slices)
{
	unsigned char *mapping;
	size_t offset;
	int fd;

	fd = p3s_open_fixture("/dev/iommu_dma_mmap_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "IOMMU DMA fixture unavailable");
	ASSERT_GE(fd, 0);

	mapping = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);

	for (offset = 0; offset < BUFFER_SIZE; offset += PROCESS_PAGE_SIZE)
		EXPECT_EQ(0x51 + offset / PROCESS_PAGE_SIZE, mapping[offset]);

	munmap(mapping, BUFFER_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
