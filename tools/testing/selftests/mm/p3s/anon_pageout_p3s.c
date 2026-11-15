// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_PAGEOUT of a 4K compat process's anonymous pages installs swap PTEs
 * that preserve their contents across out-of-order slice swap-ins.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)

static unsigned long swap_free_bytes(void)
{
	unsigned long free_kb = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo = fopen("/proc/meminfo", "re");

	if (!meminfo)
		return 0;
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "SwapFree: %lu kB", &free_kb) == 1)
			break;
	}
	free(line);
	fclose(meminfo);
	return free_kb * 1024;
}

FIXTURE(pageout) {
	unsigned char *reservation;
	unsigned char *mapping;
};

FIXTURE_SETUP(pageout)
{
	struct procmap_fd procmap;
	unsigned int i;

	self->reservation = MAP_FAILED;
	self->mapping = MAP_FAILED;
	if (!p3s_swap_available() || swap_free_bytes() < MAPPING_SIZE)
		SKIP(return, "insufficient active swap space");
	self->mapping = map_exact_edge(MAPPING_SIZE, &self->reservation);
	ASSERT_NE(MAP_FAILED, self->mapping);
	ASSERT_TRUE(!madvise(self->mapping, MAPPING_SIZE, MADV_NOHUGEPAGE) ||
		    errno == EINVAL);
	ASSERT_EQ(1, p3s_maps_count(self->mapping, MAPPING_SIZE));
	ASSERT_EQ(0, open_self_procmap(&procmap));
	ASSERT_TRUE(find_vma_procmap(&procmap, self->mapping));
	ASSERT_EQ(MAPPING_SIZE,
		  procmap.query.vma_end - procmap.query.vma_start);
	close_procmap(&procmap);
	for (i = 0; i < TEST_PAGES; i++)
		self->mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i;
}

FIXTURE_TEARDOWN(pageout)
{
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, MAPPING_SIZE + 2 * NATIVE_PAGE_SIZE);
}

TEST_F(pageout, pageout_installs_swap_ptes)
{
	static const unsigned int order[P3S_SLICES] = { 2, 0, 3, 1 };
	unsigned long rss = 0, swap = 0;
	unsigned int base, k;
	bool paged_out;

	paged_out = pageout_reaches_swap(self->mapping, MAPPING_SIZE,
					 MAPPING_SIZE / 2);
	EXPECT_TRUE(p3s_smaps_bytes(self->mapping, MAPPING_SIZE, "Rss", &rss));
	EXPECT_TRUE(p3s_smaps_bytes(self->mapping, MAPPING_SIZE, "Swap",
				    &swap));
	TH_LOG("Swap: %lu, Rss: %lu bytes", swap, rss);
	ASSERT_TRUE(paged_out);
	EXPECT_LE(rss, MAPPING_SIZE / 2);

	for (base = 0; base < TEST_PAGES; base += P3S_SLICES) {
		for (k = 0; k < P3S_SLICES; k++) {
			unsigned int idx = base + order[k];

			EXPECT_EQ((unsigned char)(0x40 + idx),
				  self->mapping[idx * PROCESS_PAGE_SIZE]);
		}
	}
}

P3S_HARNESS_4KB_MAIN
