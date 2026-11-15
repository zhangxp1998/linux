// SPDX-License-Identifier: GPL-2.0
/* A moved swap PTE must retain its original 4K offset in the native folio. */
#define _GNU_SOURCE
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define LENGTH (2 * NATIVE_PAGE_SIZE)

static bool swapped_range(const unsigned char *base)
{
	size_t off;

	for (off = 0; off < LENGTH; off += PROCESS_PAGE_SIZE)
		if (!p3s_page_swapped(base + off))
			return false;
	return true;
}

static bool swap_available(void)
{
	unsigned long total = 0;
	char line[128];
	FILE *file = fopen("/proc/meminfo", "re");

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file))
		if (sscanf(line, "SwapTotal: %lu kB", &total) == 1)
			break;
	fclose(file);
	return total != 0;
}

FIXTURE(swap_move) {
	unsigned char *src_mapping;
	unsigned char *dst_mapping;
	unsigned char *src;
	unsigned char *dst;
};

FIXTURE_SETUP(swap_move)
{
	unsigned int attempt;
	size_t i;

	if (!swap_available())
		SKIP(return, "no swap is configured");
	self->src = map_aligned(LENGTH, &self->src_mapping);
	self->dst = map_aligned(LENGTH + NATIVE_PAGE_SIZE,
				&self->dst_mapping);
	ASSERT_NE(self->src, MAP_FAILED);
	ASSERT_NE(self->dst, MAP_FAILED);
	for (i = 0; i < LENGTH; i++)
		self->src[i] = 0x40 + i / PROCESS_PAGE_SIZE;
	for (attempt = 0; attempt < 20; attempt++) {
		ASSERT_EQ(0, madvise(self->src, LENGTH, MADV_PAGEOUT));
		if (swapped_range(self->src))
			return;
		usleep(10000);
	}
	SKIP(return, "MADV_PAGEOUT did not place all slices in swap");
}

FIXTURE_TEARDOWN(swap_move)
{
	if (self->dst_mapping)
		munmap(self->dst_mapping, LENGTH + 2 * NATIVE_PAGE_SIZE);
	if (self->src_mapping)
		munmap(self->src_mapping, LENGTH + NATIVE_PAGE_SIZE);
}

TEST_F(swap_move, cross_slice_move_is_rejected)
{
	void *moved;
	int error;

	errno = 0;
	moved = mremap(self->src, LENGTH, LENGTH,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst + PROCESS_PAGE_SIZE);
	error = errno;
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EINVAL, error);
}

TEST_F(swap_move, same_slice_move_keeps_data)
{
	unsigned char *moved;
	size_t i;

	moved = mremap(self->src, LENGTH, LENGTH,
		       MREMAP_MAYMOVE | MREMAP_FIXED, self->dst);
	ASSERT_EQ(self->dst, moved);
	for (i = 0; i < LENGTH; i++)
		ASSERT_EQ(0x40 + i / PROCESS_PAGE_SIZE, moved[i]);
}

P3S_HARNESS_4KB_MAIN
