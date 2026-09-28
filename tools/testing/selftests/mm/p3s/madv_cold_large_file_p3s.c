// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_PAGEOUT reclaims file folios mapped by four 4KB slices.
 * The mapping also crosses a 4KB process PMD boundary; its split folio
 * may stay resident, but fully batched folios must be evicted.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_harness.h"

#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)
#define TAINT_BAD_PAGE_MASK 32UL

static int read_kernel_taint(unsigned long *taint)
{
	FILE *file;
	int rc;

	file = fopen("/proc/sys/kernel/tainted", "re");
	if (!file)
		return -1;
	rc = fscanf(file, "%lu", taint) == 1 ? 0 : -1;
	fclose(file);
	return rc;
}

static unsigned char slice_pattern(size_t offset)
{
	return ((offset / PROCESS_PAGE_SIZE) % 251) + 1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[PROCESS_PAGE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		memset(buffer, slice_pattern(offset), sizeof(buffer));
		if (pwrite(fd, buffer, sizeof(buffer), offset) !=
		    (ssize_t)sizeof(buffer))
			return -1;
	}
	if (fsync(fd))
		return -1;
	rc = posix_fadvise(fd, 0, MAP_SIZE, POSIX_FADV_DONTNEED);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

static size_t mapping_mismatches(const unsigned char *map,
				 size_t *checksum)
{
	size_t mismatches = 0;
	size_t offset;

	*checksum = 0;
	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		*checksum += map[offset];
		if (map[offset] != slice_pattern(offset))
			mismatches++;
	}
	return mismatches;
}

FIXTURE(cold_file) {
	unsigned char *map;
	int fd;
};

FIXTURE_SETUP(cold_file)
{
	char path[] = "madv-cold-large-file-p3s.XXXXXX";
	self->fd = mkostemp(path, O_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, unlink(path));
	ASSERT_EQ(0, prepare_file(self->fd));
	self->map = mmap(MAP_ADDR, MAP_SIZE, PROT_READ,
			 MAP_SHARED | MAP_FIXED_NOREPLACE, self->fd, 0);
	ASSERT_NE(self->map, MAP_FAILED);
}

FIXTURE_TEARDOWN(cold_file)
{
	munmap(self->map, MAP_SIZE);
	close(self->fd);
}

TEST_F(cold_file, pageout_evicts_fully_batched_file_slices)
{
	unsigned char vec[MAP_SIZE / PROCESS_PAGE_SIZE];
	unsigned int evicted = 0;
	unsigned long taint_before = 0, taint_after = 0;
	bool check_taint;
	size_t checksum;

	/* An unfaulted control distinguishes non-evictable filesystems. */
	ASSERT_EQ(0, mincore(self->map, MAP_SIZE, vec));
	for (size_t i = 0; i < sizeof(vec); i++)
		evicted += !(vec[i] & 1);
	if (!evicted)
		SKIP(return, "backing filesystem could not evict test pages");
	ASSERT_EQ(0, mapping_mismatches(self->map, &checksum));
	ASSERT_EQ(0, mincore(self->map, MAP_SIZE, vec));
	for (size_t i = 0; i < sizeof(vec); i++)
		ASSERT_TRUE(vec[i] & 1);
	check_taint = !read_kernel_taint(&taint_before) &&
		      !(taint_before & TAINT_BAD_PAGE_MASK);
	ASSERT_EQ(0, madvise(self->map, MAP_SIZE, MADV_COLD));
	ASSERT_EQ(0, madvise(self->map, MAP_SIZE, MADV_PAGEOUT));
	for (unsigned int attempt = 0; attempt < 100; attempt++) {
		evicted = 0;
		ASSERT_EQ(0, mincore(self->map, MAP_SIZE, vec));
		/* The final file folio straddles the process PMD boundary. */
		for (size_t i = 0; i < sizeof(vec) - P3S_SLICES; i++)
			evicted += !(vec[i] & 1);
		if (evicted)
			break;
		usleep(10000);
	}
	EXPECT_GT(evicted, 0);
	TH_LOG("evicted slices outside split PMD folio: %u", evicted);
	EXPECT_EQ(0, mapping_mismatches(self->map, &checksum));
	if (check_taint) {
		ASSERT_EQ(0, read_kernel_taint(&taint_after));
		EXPECT_EQ(0, taint_after & TAINT_BAD_PAGE_MASK);
	}
}

P3S_HARNESS_4KB_MAIN
