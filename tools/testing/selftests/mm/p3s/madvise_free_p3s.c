// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_FREE over an isolated 32-page anonymous VMA of a 4K compat process
 * marks the pages lazy-free and they keep their data until reclaim.  Lazy
 * freeing requires coverage of every existing mapping of the folio;
 * unpopulated slices do not prevent lazyfree.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sched.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * NATIVE_PAGE_SIZE)

/* Observe the local lazyfree batch without changing production semantics. */
static bool flush_lazyfree(void *scratch)
{
	return madvise(scratch, NATIVE_PAGE_SIZE, MADV_COLD) == 0;
}

struct partial_result {
	bool accounted;
	bool preserved;
};

static struct partial_result partial_mapping(unsigned int nr, void *scratch)
{
	struct partial_result result = {};
	unsigned char *area, *page;
	unsigned long lazyfree = 0;
	uintptr_t base;
	unsigned int i;
	int ret;

	area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		return result;
	base = ((uintptr_t)area + 2 * NATIVE_PAGE_SIZE - 1) &
	       ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	page = mmap((void *)base, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (page == MAP_FAILED)
		goto out;
	for (i = 0; i < nr; i++)
		page[i * PROCESS_PAGE_SIZE] = 0x60 + i;
	ret = madvise(page, NATIVE_PAGE_SIZE, MADV_FREE);
	result.accounted = !ret && flush_lazyfree(scratch) &&
		p3s_smaps_bytes(page, 1, "LazyFree", &lazyfree) &&
		lazyfree == nr * PROCESS_PAGE_SIZE;
	result.preserved = true;
	for (i = 0; i < nr; i++)
		if (page[i * PROCESS_PAGE_SIZE] != 0x60 + i)
			result.preserved = false;
out:
	munmap(area, 4 * NATIVE_PAGE_SIZE);
	return result;
}

FIXTURE(lazyfree)
{
	unsigned char *mapping;
	void *reservation;
	void *scratch;
};

FIXTURE_SETUP(lazyfree)
{
	uintptr_t base;
	unsigned int i;
	cpu_set_t cpus, one;
	int cpu;

	ASSERT_EQ(0, sched_getaffinity(0, sizeof(cpus), &cpus));
	CPU_ZERO(&one);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &cpus)) {
			CPU_SET(cpu, &one);
			break;
		}
	ASSERT_EQ(0, sched_setaffinity(0, sizeof(one), &one));
	self->scratch = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->scratch, MAP_FAILED);
	memset(self->scratch, 1, NATIVE_PAGE_SIZE);

	/* A native-aligned VMA with guard pages on both sides. */
	self->reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	base = ((uintptr_t)self->reservation + 2 * NATIVE_PAGE_SIZE - 1) &
	       ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	self->mapping = mmap((void *)base, MAPPING_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	/* THP hints are optional for compat slices, as in the original test. */
	madvise(self->mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
	for (i = 0; i < TEST_PAGES; i++)
		self->mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i;
}

FIXTURE_TEARDOWN(lazyfree)
{
	munmap(self->reservation, RESERVE_SIZE);
	munmap(self->scratch, NATIVE_PAGE_SIZE);
}

TEST_F(lazyfree, one_slice_does_not_lazyfree_full_tuple)
{
	unsigned long lazyfree = 0;

	ASSERT_EQ(0, madvise(self->mapping, PROCESS_PAGE_SIZE, MADV_FREE));
	ASSERT_TRUE(flush_lazyfree(self->scratch));
	ASSERT_TRUE(p3s_smaps_bytes(self->mapping, 1, "LazyFree", &lazyfree));
	EXPECT_EQ(0, lazyfree);
}

TEST_F(lazyfree, full_mapping_is_lazyfree_accounted)
{
	unsigned long lazyfree = 0;

	ASSERT_EQ(0, madvise(self->mapping, MAPPING_SIZE, MADV_FREE));
	ASSERT_TRUE(flush_lazyfree(self->scratch));
	ASSERT_TRUE(p3s_smaps_bytes(self->mapping, 1, "LazyFree", &lazyfree));
	EXPECT_EQ(MAPPING_SIZE, lazyfree);
}

TEST_F(lazyfree, full_mapping_keeps_data_before_reclaim)
{
	unsigned int i;

	ASSERT_EQ(0, madvise(self->mapping, MAPPING_SIZE, MADV_FREE));
	ASSERT_TRUE(flush_lazyfree(self->scratch));
	for (i = 0; i < TEST_PAGES; i++)
		EXPECT_EQ(0x40 + i, self->mapping[i * PROCESS_PAGE_SIZE]);
}

TEST_F(lazyfree, one_populated_slice_is_accounted)
{
	EXPECT_TRUE(partial_mapping(1, self->scratch).accounted);
}

TEST_F(lazyfree, one_populated_slice_keeps_data)
{
	EXPECT_TRUE(partial_mapping(1, self->scratch).preserved);
}

TEST_F(lazyfree, two_populated_slices_are_accounted)
{
	EXPECT_TRUE(partial_mapping(2, self->scratch).accounted);
}

TEST_F(lazyfree, two_populated_slices_keep_data)
{
	EXPECT_TRUE(partial_mapping(2, self->scratch).preserved);
}

TEST_F(lazyfree, three_populated_slices_are_accounted)
{
	EXPECT_TRUE(partial_mapping(3, self->scratch).accounted);
}

TEST_F(lazyfree, three_populated_slices_keep_data)
{
	EXPECT_TRUE(partial_mapping(3, self->scratch).preserved);
}

P3S_HARNESS_4KB_MAIN
