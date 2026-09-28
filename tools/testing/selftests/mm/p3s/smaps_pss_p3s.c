// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/*
 * A P3S file folio can be mapped by several 4K PTEs, one per slice.  Those
 * PTEs overlap at struct-page granularity but not necessarily in physical
 * bytes.  Check conservative page-level sharing, capped at each mapped slice.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "p3s_harness.h"
#include "vm_util.h"

#define USER_PAGE_SIZE 4096UL
#define MAPPING_SIZE (4 * USER_PAGE_SIZE)

struct smaps_stats {
	unsigned long rss_kb;
	unsigned long pss_kb;
	unsigned long shared_clean_kb;
	unsigned long shared_dirty_kb;
	unsigned long private_clean_kb;
	unsigned long private_dirty_kb;
};

static bool read_smaps_stats(uintptr_t address, struct smaps_stats *stats)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	memset(stats, 0, sizeof(*stats));
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;

	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, value;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_mapping)
				break;
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value) == 1)
			stats->rss_kb = value;
		else if (sscanf(line, "Pss: %lu kB", &value) == 1)
			stats->pss_kb = value;
		else if (sscanf(line, "Shared_Clean: %lu kB", &value) == 1)
			stats->shared_clean_kb = value;
		else if (sscanf(line, "Shared_Dirty: %lu kB", &value) == 1)
			stats->shared_dirty_kb = value;
		else if (sscanf(line, "Private_Clean: %lu kB", &value) == 1)
			stats->private_clean_kb = value;
		else if (sscanf(line, "Private_Dirty: %lu kB", &value) == 1)
			stats->private_dirty_kb = value;
	}

	free(line);
	fclose(file);
	return in_mapping && stats->rss_kb;
}

static bool stats_equal(const struct smaps_stats *stats,
			unsigned long rss, unsigned long pss,
			unsigned long shared, unsigned long private)
{
	return stats->rss_kb == rss && stats->pss_kb == pss &&
	       stats->shared_clean_kb + stats->shared_dirty_kb == shared &&
	       stats->private_clean_kb + stats->private_dirty_kb == private;
}

static void show_stats(const struct smaps_stats *stats)
{
	printf("# Rss=%lu Pss=%lu Shared=%lu Private=%lu kB\n",
	       stats->rss_kb, stats->pss_kb,
	       stats->shared_clean_kb + stats->shared_dirty_kb,
	       stats->private_clean_kb + stats->private_dirty_kb);
}

FIXTURE(smaps_file) {
	unsigned char *mapping;
	unsigned char *alias;
	int fd;
};

FIXTURE_SETUP(smaps_file)
{
	unsigned long i;

	ASSERT_EQ(USER_PAGE_SIZE, sysconf(_SC_PAGESIZE));
	self->alias = MAP_FAILED;
	self->fd = syscall(SYS_memfd_create, "smaps-pss-p3s", MFD_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, MAPPING_SIZE));
	self->mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, self->fd, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	for (i = 0; i < MAPPING_SIZE; i += USER_PAGE_SIZE)
		self->mapping[i] = (unsigned char)(0x40 + i / USER_PAGE_SIZE);
}

FIXTURE_TEARDOWN(smaps_file)
{
	if (self->alias != MAP_FAILED)
		munmap(self->alias, USER_PAGE_SIZE);
	if (self->mapping != MAP_FAILED)
		munmap(self->mapping, MAPPING_SIZE);
	if (self->fd >= 0)
		close(self->fd);
}

static void add_alias(FIXTURE_DATA(smaps_file) *self)
{
	self->alias = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, self->fd, 0);
	if (self->alias != MAP_FAILED)
		self->alias[0] ^= 1;
}

TEST_F(smaps_file, disjoint_slices_have_full_private_pss)
{
	struct smaps_stats stats;

	ASSERT_TRUE(read_smaps_stats((uintptr_t)self->mapping, &stats));
	show_stats(&stats);
	EXPECT_TRUE(stats_equal(&stats, 16, 16, 0, 16));
}

TEST_F(smaps_file, alias_shares_native_page_conservatively)
{
	struct smaps_stats stats;

	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	ASSERT_TRUE(read_smaps_stats((uintptr_t)self->mapping, &stats));
	show_stats(&stats);
	EXPECT_TRUE(stats_equal(&stats, 16, 12, 16, 0));
}

TEST_F(smaps_file, alias_gets_its_mapcount_share)
{
	struct smaps_stats stats;

	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	ASSERT_TRUE(read_smaps_stats((uintptr_t)self->alias, &stats));
	show_stats(&stats);
	EXPECT_TRUE(stats_equal(&stats, 4, 3, 4, 0));
}

TEST_F(smaps_file, unmapping_alias_restores_private_pss)
{
	struct smaps_stats stats;

	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	ASSERT_EQ(0, munmap(self->alias, USER_PAGE_SIZE));
	self->alias = MAP_FAILED;
	ASSERT_TRUE(read_smaps_stats((uintptr_t)self->mapping, &stats));
	show_stats(&stats);
	EXPECT_TRUE(stats_equal(&stats, 16, 16, 0, 16));
}

P3S_HARNESS_4KB_MAIN
