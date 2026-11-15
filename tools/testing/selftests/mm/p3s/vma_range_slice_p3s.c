// SPDX-License-Identifier: GPL-2.0
/* 4KB file-backed VMA split/merge preserves offsets and refaulted bytes. */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define FILE_OFFSET (2 * PROCESS_PAGE_SIZE)

static bool contents_match(const unsigned char *mapping)
{
	unsigned int slice;

	for (slice = 0; slice < P3S_SLICES; slice++)
		if (mapping[slice * PROCESS_PAGE_SIZE] != 0x41 + slice)
			return false;
	return true;
}

FIXTURE(vma_range) {
	int fd;
};

FIXTURE_VARIANT(vma_range) {
	unsigned int which;
};

FIXTURE_VARIANT_ADD(vma_range, first_slice_protect_restore) { .which = 0 };
FIXTURE_VARIANT_ADD(vma_range, suffix_read_only) { .which = 1 };
FIXTURE_VARIANT_ADD(vma_range, prefix_read_only) { .which = 2 };
FIXTURE_VARIANT_ADD(vma_range, middle_protect_restore) { .which = 3 };

FIXTURE_SETUP(vma_range)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	unsigned int slice;

	self->fd = syscall(__NR_memfd_create, "vma-range-slice", 0);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, FILE_OFFSET + NATIVE_PAGE_SIZE));
	for (slice = 0; slice < P3S_SLICES; slice++) {
		memset(page, 0x41 + slice, sizeof(page));
		ASSERT_EQ((ssize_t)sizeof(page),
			  pwrite(self->fd, page, sizeof(page),
				 FILE_OFFSET + slice * PROCESS_PAGE_SIZE));
	}
}

FIXTURE_TEARDOWN(vma_range)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(vma_range, split_and_merge_keep_file_slices)
{
	void *reservation;
	unsigned char *mapping;
	unsigned int segments;
	unsigned long offset;
	uintptr_t aligned;

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)aligned, NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		       self->fd, FILE_OFFSET);
	ASSERT_EQ((void *)aligned, mapping);

	switch (variant->which) {
	case 0:
		ASSERT_EQ(0, mprotect(mapping, PROCESS_PAGE_SIZE, PROT_READ));
		ASSERT_EQ(0, mprotect(mapping, PROCESS_PAGE_SIZE,
				      PROT_READ | PROT_WRITE));
		break;
	case 1:
		ASSERT_EQ(0, mprotect(mapping + PROCESS_PAGE_SIZE,
				      3 * PROCESS_PAGE_SIZE, PROT_READ));
		ASSERT_EQ(0, mprotect(mapping, PROCESS_PAGE_SIZE, PROT_READ));
		break;
	case 2:
		ASSERT_EQ(0, mprotect(mapping, 3 * PROCESS_PAGE_SIZE, PROT_READ));
		ASSERT_EQ(0, mprotect(mapping + 3 * PROCESS_PAGE_SIZE,
				      PROCESS_PAGE_SIZE, PROT_READ));
		break;
	case 3:
		ASSERT_EQ(0, mprotect(mapping + PROCESS_PAGE_SIZE,
				      2 * PROCESS_PAGE_SIZE, PROT_READ));
		ASSERT_EQ(0, mprotect(mapping + PROCESS_PAGE_SIZE,
				      2 * PROCESS_PAGE_SIZE,
				      PROT_READ | PROT_WRITE));
		break;
	default:
		ASSERT_FALSE(true);
	}

	ASSERT_TRUE(p3s_maps_range(mapping, NATIVE_PAGE_SIZE,
				   &segments, &offset));
	EXPECT_EQ(FILE_OFFSET, offset);
	EXPECT_EQ(1, segments);
	EXPECT_TRUE(contents_match(mapping));
	ASSERT_EQ(0, madvise(mapping, NATIVE_PAGE_SIZE, MADV_DONTNEED));
	EXPECT_TRUE(contents_match(mapping));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
