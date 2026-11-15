// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

FIXTURE(partial_anon_reclaim) {
	unsigned char *area;
};

FIXTURE_SETUP(partial_anon_reclaim)
{
	self->area = MAP_FAILED;
}

FIXTURE_TEARDOWN(partial_anon_reclaim)
{
	if (self->area != MAP_FAILED)
		munmap(self->area, 3 * NATIVE_PAGE_SIZE);
}

static unsigned char *alloc_window(unsigned char **area)
{
	unsigned char *p;

	*area = MAP_FAILED;
	p = map_exact_edge(NATIVE_PAGE_SIZE, area);
	if (p != MAP_FAILED)
		memset(p, 0x57, NATIVE_PAGE_SIZE);
	return p;
}

TEST_F(partial_anon_reclaim, p3s_partial_anon_reclaim)
{
	unsigned char *p;
	unsigned long before = 0, after = 0;
	bool bytes = true;
	unsigned int i;

	if (!p3s_swap_available())
		SKIP(return, "no active swap device");
	p = alloc_window(&self->area);
	ASSERT_NE(MAP_FAILED, p);
	if (!pageout_reaches_swap(p, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE))
		SKIP(return, "whole-folio PAGEOUT positive control unavailable");
	for (i = 0; i < NATIVE_PAGE_SIZE; i++)
		bytes &= p[i] == 0x57;
	munmap(self->area, 3 * NATIVE_PAGE_SIZE);
	self->area = MAP_FAILED;

	EXPECT_TRUE(bytes);
	p = alloc_window(&self->area);
	ASSERT_NE(MAP_FAILED, p);
	ASSERT_EQ(0, munmap(p + 3 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE));
	ASSERT_TRUE(p3s_smaps_bytes(p, 1, "Referenced", &before));
	ASSERT_EQ(0, madvise(p, 3 * PROCESS_PAGE_SIZE, MADV_COLD));
	ASSERT_TRUE(p3s_smaps_bytes(p, 1, "Referenced", &after));
	ksft_print_msg("partial folio Referenced: before=%lu after=%lu bytes\n",
		       before, after);
	EXPECT_TRUE(before && !after);
	EXPECT_TRUE(pageout_reaches_swap(p, 3 * PROCESS_PAGE_SIZE,
					 3 * PROCESS_PAGE_SIZE));
	bytes = true;
	for (i = 0; i < 3 * PROCESS_PAGE_SIZE; i++)
		bytes &= p[i] == 0x57;
	EXPECT_TRUE(bytes);
}

P3S_HARNESS_4KB_MAIN
