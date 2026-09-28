// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/* Check that pagemap conservatively detects aliases of a native file page. */

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
#include <sys/syscall.h>
#include <unistd.h>

#include "p3s_harness.h"
#include "vm_util.h"

#define USER_PAGE_SIZE 4096UL
#define NR_SLICES 4
#define MAPPING_SIZE (NR_SLICES * USER_PAGE_SIZE)
static int pagemap_fd = -1;

static int read_pagemap(void *address, uint64_t *entry)
{
	if (pagemap_fd < 0)
		pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0)
		return -1;
	*entry = pagemap_get_entry(pagemap_fd, address);
	return 0;
}

static bool check_exclusive(unsigned char *mapping, unsigned int exclusive_mask)
{
	unsigned int observed = 0;
	unsigned int i;

	for (i = 0; i < NR_SLICES; i++) {
		uint64_t entry;

		if (read_pagemap(mapping + i * USER_PAGE_SIZE, &entry)) {
			perror("read pagemap");
			return false;
		}
		if ((entry & (PM_PRESENT | PM_FILE)) != (PM_PRESENT | PM_FILE)) {
			printf("# slice %u missing present/file flags: %#llx\n", i,
			       (unsigned long long)entry);
			return false;
		}
		if (entry & PM_MMAP_EXCLUSIVE)
			observed |= 1U << i;
	}
	printf("# exclusive mask: expected=%#x observed=%#x\n",
	       exclusive_mask, observed);
	return observed == exclusive_mask;
}

FIXTURE(pagemap_file) {
	unsigned char *mapping;
	unsigned char *alias;
	int fd;
};

FIXTURE_SETUP(pagemap_file)
{
	unsigned int i;

	ASSERT_EQ(USER_PAGE_SIZE, sysconf(_SC_PAGESIZE));
	self->alias = MAP_FAILED;
	self->fd = syscall(SYS_memfd_create, "pagemap-exclusive-p3s", MFD_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, MAPPING_SIZE));
	self->mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, self->fd, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	for (i = 0; i < NR_SLICES; i++)
		self->mapping[i * USER_PAGE_SIZE] = (unsigned char)i;
}

FIXTURE_TEARDOWN(pagemap_file)
{
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	if (self->alias != MAP_FAILED)
		munmap(self->alias, USER_PAGE_SIZE);
	if (self->mapping != MAP_FAILED)
		munmap(self->mapping, MAPPING_SIZE);
	if (self->fd >= 0)
		close(self->fd);
}

static void add_alias(FIXTURE_DATA(pagemap_file) *self)
{
	self->alias = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, self->fd, 0);
	if (self->alias != MAP_FAILED)
		self->alias[0] ^= 1;
}

TEST_F(pagemap_file, disjoint_slices_are_exclusive)
{
	EXPECT_TRUE(check_exclusive(self->mapping, 0xf));
}

TEST_F(pagemap_file, alias_clears_window_exclusivity)
{
	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	EXPECT_TRUE(check_exclusive(self->mapping, 0));
}

TEST_F(pagemap_file, alias_is_not_exclusive)
{
	uint64_t entry;

	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	ASSERT_EQ(0, read_pagemap(self->alias, &entry));
	EXPECT_EQ(0, entry & PM_MMAP_EXCLUSIVE);
}

TEST_F(pagemap_file, unmapping_alias_restores_exclusivity)
{
	add_alias(self);
	ASSERT_NE(self->alias, MAP_FAILED);
	ASSERT_EQ(0, munmap(self->alias, USER_PAGE_SIZE));
	self->alias = MAP_FAILED;
	EXPECT_TRUE(check_exclusive(self->mapping, 0xf));
}

P3S_HARNESS_4KB_MAIN
