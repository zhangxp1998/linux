// SPDX-License-Identifier: GPL-2.0
/* Locking zero-page siblings keeps their shared folio unevictable. */
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define KPF_UNEVICTABLE (1ULL << 18)

/* Flush the per-CPU mlock batch using only test-owned mappings. */
static bool drain_mlock_batch(struct __test_metadata *_metadata)
{
	size_t bytes = 128 * NATIVE_PAGE_SIZE;
	unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_NE(MAP_FAILED, p);
	memset(p, 0x69, bytes);
	if (mlock(p, bytes)) {
		munmap(p, bytes);
		return false;
	}
	ASSERT_EQ(0, munlock(p, bytes));
	munmap(p, bytes);
	return true;
}

FIXTURE(zero_sibling) {
	unsigned char *area;
	unsigned char *map;
	int flags_fd;
};

FIXTURE_SETUP(zero_sibling)
{
	self->flags_fd = p3s_open_fixture("/proc/kpageflags", O_RDONLY);
	if (self->flags_fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "kpageflags unavailable");
	ASSERT_GE(self->flags_fd, 0);
	self->map = map_aligned(NATIVE_PAGE_SIZE, &self->area);
	ASSERT_NE(MAP_FAILED, self->map);
}

FIXTURE_TEARDOWN(zero_sibling)
{
	if (self->area)
		munmap(self->area, 2 * NATIVE_PAGE_SIZE);
	if (self->flags_fd >= 0)
		close(self->flags_fd);
}

TEST_F(zero_sibling, partial_unlock_keeps_folio_unevictable)
{
	unsigned char *map = self->map;
	uint64_t pfn, sibling, entry, flags;
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		FORCE_READ(map[i * PROCESS_PAGE_SIZE]);
	if (mlock(map, NATIVE_PAGE_SIZE))
		SKIP(return, "mlock: %s", strerror(errno));
	if (!drain_mlock_batch(_metadata))
		SKIP(return, "mlock batch exceeds memlock limit");
	if (!p3s_pfn(map, &pfn))
		SKIP(return, "pagemap PFNs are unavailable");
	for (i = 1; i < P3S_SLICES; i++) {
		ASSERT_TRUE(p3s_pfn(map + i * PROCESS_PAGE_SIZE, &sibling));
		EXPECT_EQ(pfn, sibling);
	}
	ASSERT_TRUE(p3s_pagemap_entry(map, &entry));
	ASSERT_TRUE(entry & PM_PRESENT);
	/* /proc/kpageflags is indexed in the reader's 4KB frames. */
	ASSERT_EQ(0, pageflags_get(PAGEMAP_PFN(entry), self->flags_fd, &flags));
	EXPECT_TRUE(flags & KPF_UNEVICTABLE);
	ASSERT_EQ(0, munlock(map, PROCESS_PAGE_SIZE));
	if (!drain_mlock_batch(_metadata))
		SKIP(return, "mlock batch exceeds memlock limit");
	ASSERT_EQ(0, pageflags_get(PAGEMAP_PFN(entry), self->flags_fd, &flags));
	EXPECT_TRUE(flags & KPF_UNEVICTABLE);
	ASSERT_EQ(0, munlock(map + PROCESS_PAGE_SIZE,
			     NATIVE_PAGE_SIZE - PROCESS_PAGE_SIZE));
	if (!drain_mlock_batch(_metadata))
		SKIP(return, "mlock batch exceeds memlock limit");
	ASSERT_EQ(0, pageflags_get(PAGEMAP_PFN(entry), self->flags_fd, &flags));
	EXPECT_FALSE(flags & KPF_UNEVICTABLE);
}

P3S_HARNESS_4KB_MAIN
