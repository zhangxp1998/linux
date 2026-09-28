// SPDX-License-Identifier: GPL-2.0
/* Validate PAGEMAP_SCAN argument geometry in a 4K P3S process. */
#define _GNU_SOURCE

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

static void init_scan(struct pm_scan_arg *arg, void *start, size_t length,
		      struct page_region *regions, size_t nr_regions)
{
	memset(arg, 0, sizeof(*arg));
	arg->size = sizeof(*arg);
	arg->start = (uintptr_t)start;
	arg->end = arg->start + length;
	arg->vec = (uintptr_t)regions;
	arg->vec_len = nr_regions;
	arg->category_anyof_mask = PAGE_IS_PRESENT;
	arg->return_mask = PAGE_IS_PRESENT;
}

static int scan(int fd, struct pm_scan_arg *arg)
{
	errno = 0;
	return ioctl(fd, PAGEMAP_SCAN, arg);
}

FIXTURE(pagemap_scan) {
	int fd;
	unsigned char *mapping;
	unsigned char *reservation;
	struct page_region regions[8];
	struct pm_scan_arg arg;
};

FIXTURE_SETUP(pagemap_scan)
{
	self->fd = -1;
	self->reservation = MAP_FAILED;
	self->fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	self->mapping = map_aligned(NATIVE_PAGE_SIZE, &self->reservation);
	ASSERT_NE(self->mapping, MAP_FAILED);
	self->mapping[0] = 1;
	self->mapping[2 * PROCESS_PAGE_SIZE] = 2;
	memset(self->regions, 0, sizeof(self->regions));
}

FIXTURE_TEARDOWN(pagemap_scan)
{
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, 2 * NATIVE_PAGE_SIZE);
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(pagemap_scan, present_pages_are_separate_ranges)
{
	init_scan(&self->arg, self->mapping, 3 * PROCESS_PAGE_SIZE + 1,
		  self->regions, ARRAY_SIZE(self->regions));
	EXPECT_EQ(2, scan(self->fd, &self->arg));
}

TEST_F(pagemap_scan, end_rounds_to_process_page)
{
	init_scan(&self->arg, self->mapping, PROCESS_PAGE_SIZE + 1,
		  self->regions, ARRAY_SIZE(self->regions));
	ASSERT_EQ(1, scan(self->fd, &self->arg));
	EXPECT_EQ((uintptr_t)self->mapping + 2 * PROCESS_PAGE_SIZE,
		  self->arg.walk_end);
}

TEST_F(pagemap_scan, start_accepts_process_page_offset)
{
	init_scan(&self->arg, self->mapping + PROCESS_PAGE_SIZE,
		  PROCESS_PAGE_SIZE, self->regions, ARRAY_SIZE(self->regions));
	ASSERT_GE(scan(self->fd, &self->arg), 0);
	EXPECT_EQ((uintptr_t)self->mapping + 2 * PROCESS_PAGE_SIZE,
		  self->arg.walk_end);
}

TEST_F(pagemap_scan, ranges_use_process_page_boundaries)
{
	init_scan(&self->arg, self->mapping, 3 * PROCESS_PAGE_SIZE + 1,
		  self->regions, ARRAY_SIZE(self->regions));
	ASSERT_EQ(2, scan(self->fd, &self->arg));
	EXPECT_EQ((uintptr_t)self->mapping, self->regions[0].start);
	EXPECT_EQ((uintptr_t)self->mapping + PROCESS_PAGE_SIZE,
		  self->regions[0].end);
	EXPECT_EQ((uintptr_t)self->mapping + 2 * PROCESS_PAGE_SIZE,
		  self->regions[1].start);
	EXPECT_EQ((uintptr_t)self->mapping + 3 * PROCESS_PAGE_SIZE,
		  self->regions[1].end);
}

TEST_F(pagemap_scan, max_pages_limits_results)
{
	init_scan(&self->arg, self->mapping, 3 * PROCESS_PAGE_SIZE,
		  self->regions, ARRAY_SIZE(self->regions));
	self->arg.max_pages = 1;
	EXPECT_EQ(1, scan(self->fd, &self->arg));
}

TEST_F(pagemap_scan, max_pages_stops_before_next_match)
{
	init_scan(&self->arg, self->mapping, 3 * PROCESS_PAGE_SIZE,
		  self->regions, ARRAY_SIZE(self->regions));
	self->arg.max_pages = 1;
	ASSERT_EQ(1, scan(self->fd, &self->arg));
	EXPECT_EQ((uintptr_t)self->mapping + 2 * PROCESS_PAGE_SIZE,
		  self->arg.walk_end);
}

TEST_F(pagemap_scan, allow_empty_output_vector)
{
	init_scan(&self->arg, self->mapping, PROCESS_PAGE_SIZE, NULL, 0);
	EXPECT_EQ(0, scan(self->fd, &self->arg));
	EXPECT_EQ((uintptr_t)self->mapping + PROCESS_PAGE_SIZE,
		  self->arg.walk_end);
}

P3S_HARNESS_4KB_MAIN
