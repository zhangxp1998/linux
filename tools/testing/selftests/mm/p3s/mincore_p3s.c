// SPDX-License-Identifier: GPL-2.0
/*
 * mincore() reports process-page-sized vectors in both native and compat
 * processes.  Check alignment, rounding, cache lookup without PTEs and the
 * anonymous mapping residency lifecycle without assuming one fault populates
 * exactly one process page.
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <sys/mman.h>

#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"

#define MAPPING_SIZE	(4 * NATIVE_PAGE_SIZE)
#define SENTINEL	0xa5

#define page_size ((size_t)getpagesize())

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool entries_are_resident(const unsigned char *vec, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (vec[i] == SENTINEL || !(vec[i] & 1))
			return false;
	}
	return vec[nr] == SENTINEL;
}

static bool test_4k_only_alignment(unsigned char *large_page_aligned)
{
	unsigned char vec = SENTINEL;
	void *start = large_page_aligned + PROCESS_PAGE_SIZE;
	int ret;

	errno = 0;
	ret = mincore(start, PROCESS_PAGE_SIZE, &vec);
	if (page_size == NATIVE_PAGE_SIZE)
		return ret == -1 && errno == EINVAL && vec == SENTINEL;
	if (ret) {
		ksft_print_msg("mincore(4K-only aligned) failed: %s\n",
			       strerror(errno));
		return false;
	}
	return vec & 1;
}

static bool test_vector_granularity(unsigned char *large_page_aligned)
{
	unsigned char vec[8];

	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(large_page_aligned, NATIVE_PAGE_SIZE, vec)) {
		ksft_print_msg("mincore(16K) failed: %s\n", strerror(errno));
		return false;
	}
	ksft_print_msg("16K vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	return entries_are_resident(vec, NATIVE_PAGE_SIZE / page_size);
}

static bool test_partial_length_rounding(unsigned char *large_page_aligned)
{
	const size_t length = page_size + 1;
	unsigned char vec[8];

	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(large_page_aligned, length, vec)) {
		ksft_print_msg("mincore(partial length) failed: %s\n",
			       strerror(errno));
		return false;
	}
	ksft_print_msg("partial vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	return entries_are_resident(vec, 2);
}

static bool test_file_page_cache_index(void)
{
	unsigned char vec[8];
	unsigned char *contents = NULL;
	unsigned char *mapping = MAP_FAILED;
	bool passed = false;
	ssize_t written;
	int fd = -1;

	fd = memfd_create("mincore-p3s", MFD_CLOEXEC);
	if (fd < 0) {
		ksft_print_msg("memfd_create failed: %s\n", strerror(errno));
		goto out;
	}
	contents = malloc(NATIVE_PAGE_SIZE);
	if (!contents) {
		ksft_print_msg("malloc failed\n");
		goto out;
	}
	memset(contents, 0x6d, NATIVE_PAGE_SIZE);
	written = pwrite(fd, contents, NATIVE_PAGE_SIZE, 0);
	if (written != (ssize_t)NATIVE_PAGE_SIZE) {
		ksft_print_msg("pwrite returned %zd: %s\n", written,
			       strerror(errno));
		goto out;
	}

	/* Leave the PTEs absent so mincore() has to query the page cache. */
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		ksft_print_msg("file mmap failed: %s\n", strerror(errno));
		goto out;
	}
	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(mapping, NATIVE_PAGE_SIZE, vec)) {
		ksft_print_msg("file mincore failed: %s\n", strerror(errno));
		goto out;
	}
	ksft_print_msg("file vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	passed = entries_are_resident(vec, NATIVE_PAGE_SIZE / page_size);

out:
	if (mapping != MAP_FAILED)
		munmap(mapping, NATIVE_PAGE_SIZE);
	free(contents);
	if (fd >= 0)
		close(fd);
	return passed;
}

static bool entries_are_absent(const unsigned char *vec, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		if (vec[i] == SENTINEL || (vec[i] & 1))
			return false;
	}
	return vec[nr] == SENTINEL;
}

FIXTURE(mincore_aligned) {
	unsigned char *mapping;
	unsigned char *aligned;
};

FIXTURE_SETUP(mincore_aligned)
{
	unsigned int i;

	self->mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	self->aligned = (unsigned char *)align_up((uintptr_t)self->mapping,
						       NATIVE_PAGE_SIZE);
	ASSERT_LE(self->aligned + 2 * NATIVE_PAGE_SIZE,
		  self->mapping + MAPPING_SIZE);
	for (i = 0; i < 2 * NATIVE_PAGE_SIZE; i += page_size)
		self->aligned[i] = 0x40;
}

FIXTURE_TEARDOWN(mincore_aligned)
{
	munmap(self->mapping, MAPPING_SIZE);
}

TEST_F(mincore_aligned, alignment_follows_process_page)
{
	EXPECT_TRUE(test_4k_only_alignment(self->aligned));
}

TEST_F(mincore_aligned, vector_uses_process_page_granularity)
{
	EXPECT_TRUE(test_vector_granularity(self->aligned));
}

TEST_F(mincore_aligned, partial_length_rounds_to_process_page)
{
	EXPECT_TRUE(test_partial_length_rounding(self->aligned));
}

TEST(file_cache_index_without_present_ptes)
{
	EXPECT_TRUE(test_file_page_cache_index());
}

FIXTURE(mincore_residency) {
	unsigned char *mapping;
};

FIXTURE_SETUP(mincore_residency)
{
	self->mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	/* Keep readahead/THP policy out of this base-page API test. */
	if (madvise(self->mapping, MAPPING_SIZE, MADV_NOHUGEPAGE))
		ASSERT_EQ(EINVAL, errno);
}

FIXTURE_TEARDOWN(mincore_residency)
{
	munmap(self->mapping, MAPPING_SIZE);
}

static void populate_residency(unsigned char *mapping)
{
	size_t i;

	for (i = 0; i < MAPPING_SIZE; i += page_size)
		mapping[i] = 0x5a;
}

TEST_F(mincore_residency, fresh_anonymous_mapping_is_absent)
{
	unsigned char vec[MAPPING_SIZE / PROCESS_PAGE_SIZE + 1];

	memset(vec, SENTINEL, sizeof(vec));
	ASSERT_EQ(0, mincore(self->mapping, MAPPING_SIZE, vec));
	EXPECT_TRUE(entries_are_absent(vec, MAPPING_SIZE / page_size));
}

TEST_F(mincore_residency, one_fault_leaves_distant_page_absent)
{
	unsigned char vec[MAPPING_SIZE / PROCESS_PAGE_SIZE + 1];
	size_t nr = MAPPING_SIZE / page_size;

	self->mapping[0] = 0x5a;
	memset(vec, SENTINEL, sizeof(vec));
	ASSERT_EQ(0, mincore(self->mapping, MAPPING_SIZE, vec));
	EXPECT_NE(SENTINEL, vec[0]);
	EXPECT_TRUE(vec[0] & 1);
	EXPECT_FALSE(vec[nr - 1] & 1);
	EXPECT_EQ(SENTINEL, vec[nr]);
}

TEST_F(mincore_residency, touching_each_page_populates_vector)
{
	unsigned char vec[MAPPING_SIZE / PROCESS_PAGE_SIZE + 1];

	populate_residency(self->mapping);
	memset(vec, SENTINEL, sizeof(vec));
	ASSERT_EQ(0, mincore(self->mapping, MAPPING_SIZE, vec));
	EXPECT_TRUE(entries_are_resident(vec, MAPPING_SIZE / page_size));
}

TEST_F(mincore_residency, prot_none_keeps_residency)
{
	unsigned char vec[MAPPING_SIZE / PROCESS_PAGE_SIZE + 1];

	populate_residency(self->mapping);
	ASSERT_EQ(0, mprotect(self->mapping, MAPPING_SIZE, PROT_NONE));
	memset(vec, SENTINEL, sizeof(vec));
	ASSERT_EQ(0, mincore(self->mapping, MAPPING_SIZE, vec));
	EXPECT_TRUE(entries_are_resident(vec, MAPPING_SIZE / page_size));
}

TEST_F(mincore_residency, dontneed_clears_anonymous_vector)
{
	unsigned char vec[MAPPING_SIZE / PROCESS_PAGE_SIZE + 1];

	populate_residency(self->mapping);
	ASSERT_EQ(0, madvise(self->mapping, MAPPING_SIZE, MADV_DONTNEED));
	memset(vec, SENTINEL, sizeof(vec));
	ASSERT_EQ(0, mincore(self->mapping, MAPPING_SIZE, vec));
	EXPECT_TRUE(entries_are_absent(vec, MAPPING_SIZE / page_size));
}

P3S_HARNESS_4KB_MAIN
