// SPDX-License-Identifier: GPL-2.0
/* Moving zero-page and DONTUNMAP anonymous slices. */
#define _GNU_SOURCE
#include "p3s_slice_test.h"
#include "p3s_harness.h"

TEST(zero_page_slices_mremap)
{
	unsigned char *src_reservation = NULL, *dst_reservation = NULL;
	unsigned char *src, *dst_area, *dst;
	uint64_t pfn[P3S_SLICES];
	unsigned char value = 0;
	unsigned int slice;
	void *moved;

	src = map_aligned(NATIVE_PAGE_SIZE, &src_reservation);
	ASSERT_NE(MAP_FAILED, src);
	dst_area = map_aligned(3 * NATIVE_PAGE_SIZE, &dst_reservation);
	if (dst_area == MAP_FAILED) {
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
		ASSERT_NE(MAP_FAILED, dst_area);
	}
	dst = dst_area + PROCESS_PAGE_SIZE;
	for (slice = 0; slice < P3S_SLICES; slice++)
		value |= src[slice * PROCESS_PAGE_SIZE];
	EXPECT_EQ(0, value);
	errno = 0;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EINVAL, errno);
	dst = dst_area;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	ASSERT_EQ(dst, moved);
	for (slice = 0; slice < P3S_SLICES; slice++)
		EXPECT_EQ(0, dst[slice * PROCESS_PAGE_SIZE]);
	fill_slices(dst, 0x55);
	ASSERT_TRUE(check_slices(dst, 0x55));
	ASSERT_TRUE(read_pfns(dst, pfn));
	if (!pfn[0]) {
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
		munmap(dst_reservation, 4 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
	munmap(dst_reservation, 4 * NATIVE_PAGE_SIZE);
}

TEST(dontunmap_mremap)
{
	unsigned char *src_reservation = NULL, *dst_reservation = NULL;
	unsigned char *src, *dst_area;
	unsigned int round;

	src = map_aligned(NATIVE_PAGE_SIZE, &src_reservation);
	ASSERT_NE(MAP_FAILED, src);
	dst_area = map_aligned(NATIVE_PAGE_SIZE, &dst_reservation);
	if (dst_area == MAP_FAILED) {
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
		ASSERT_NE(MAP_FAILED, dst_area);
	}

	for (round = 0; round < 128; round++) {
		unsigned int src_slice = round & (P3S_SLICES - 1);
		unsigned int dst_slice = (src_slice + 1) & (P3S_SLICES - 1);
		unsigned char *src_page = src + src_slice * PROCESS_PAGE_SIZE;
		unsigned char *dst_page = dst_area + dst_slice * PROCESS_PAGE_SIZE;
		unsigned char value = 0x41 + src_slice;
		void *moved;

		memset(src_page, value, PROCESS_PAGE_SIZE);
		errno = 0;
		moved = mremap(src_page, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			       MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
			       dst_page);
		ASSERT_EQ(MAP_FAILED, moved);
		ASSERT_EQ(EINVAL, errno);
		ASSERT_EQ(value, src_page[0]);

		dst_page = dst_area + src_slice * PROCESS_PAGE_SIZE;
		moved = mremap(src_page, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			       MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
			       dst_page);
		ASSERT_EQ(dst_page, moved);
		EXPECT_EQ(value, dst_page[0]);
		EXPECT_EQ(value, dst_page[PROCESS_PAGE_SIZE - 1]);
		EXPECT_EQ(0, src_page[0]);
	}
	munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
	munmap(dst_reservation, 2 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
