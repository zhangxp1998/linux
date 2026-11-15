// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process can map any 4K offset of a file.  Two adjacent file
 * mappings may only be merged into one VMA when the second one continues the
 * first one's file offsets exactly; mappings whose 16K file folio indices
 * happen to line up but whose 4K offsets do not must stay separate VMAs,
 * otherwise the merged VMA maps the wrong file pages.  Every 4K page of the
 * test file holds its own index, so a wrong merge shows up as wrong data.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"
#include "p3s_test.h"
#include "p3s_harness.h"

#define FILE_PAGES	64
#define UNIT		PROCESS_PAGE_SIZE

static int file_fd;

FIXTURE(file_slice) {
	int fd;
};

FIXTURE_SETUP(file_slice)
{
	static unsigned char page[UNIT];
	uint32_t i;

	self->fd = syscall(__NR_memfd_create, "vma_merge_file_slice", 0);
	ASSERT_GE(self->fd, 0);
	file_fd = self->fd;
	for (i = 0; i < FILE_PAGES; i++) {
		memset(page, 0, sizeof(page));
		memcpy(page, &i, sizeof(i));
		ASSERT_TRUE(pwrite_full(self->fd, page, UNIT, (off_t)i * UNIT));
	}
}

FIXTURE_TEARDOWN(file_slice)
{
	if (self->fd >= 0)
		close(self->fd);
}

static uint32_t page_id(const unsigned char *page)
{
	uint32_t id;

	memcpy(&id, page, sizeof(id));
	return id;
}

/* Every 4K page in [addr, addr + pages * 4K) maps file page first + i. */
static bool maps_file_pages(const unsigned char *addr, unsigned int pages,
			    unsigned int first, const char *what)
{
	unsigned int i;
	bool ok = true;

	for (i = 0; i < pages; i++) {
		uint32_t id = page_id(addr + i * UNIT);

		if (id != first + i) {
			ksft_print_msg("%s: page %u maps file page %u, expected %u\n",
				       what, i, id, first + i);
			ok = false;
		}
	}
	return ok;
}

/* A PROT_NONE reservation of @len bytes aligned to a native page. */
static unsigned char *reserve(unsigned long len, unsigned char **raw)
{
	unsigned char *p = mmap(NULL, len + 2 * NATIVE_PAGE_SIZE, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("reserve: %s\n", strerror(errno));
	*raw = p;
	return (unsigned char *)(((uintptr_t)p + 2 * NATIVE_PAGE_SIZE - 1) &
				 ~(NATIVE_PAGE_SIZE - 1));
}

static unsigned char *map_at(unsigned char *addr, unsigned int pages,
			     unsigned int file_page, int prot)
{
	unsigned char *p = mmap(addr, pages * UNIT, prot, MAP_SHARED | MAP_FIXED,
				file_fd, (off_t)file_page * UNIT);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("mmap(%u pages @ file page %u): %s\n",
				   pages, file_page, strerror(errno));
	return p;
}

/* 16K at file page 0, then 4K at file page 5 right after it. */
static bool case_fixed_after(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	bool ok;

	map_at(base, 4, 0, PROT_READ);
	map_at(base + 4 * UNIT, 1, 5, PROT_READ);
	ok = maps_file_pages(base, 4, 0, "first mapping");
	ok = maps_file_pages(base + 4 * UNIT, 1, 5, "second mapping") && ok;
	*count = p3s_maps_count(base, 5 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

/* 16K at file page 8, then 16K at file page 5 right before it. */
static bool case_before(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	bool ok;

	map_at(base + 4 * UNIT, 4, 8, PROT_READ);
	map_at(base, 4, 5, PROT_READ);
	ok = maps_file_pages(base, 4, 5, "lower mapping");
	ok = maps_file_pages(base + 4 * UNIT, 4, 8, "upper mapping") && ok;
	*count = p3s_maps_count(base, 8 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

/* The same 4K of the file mapped twice side by side ("magic ring buffer"). */
static bool case_mirror(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	bool ok;

	map_at(base, 1, 0, PROT_READ | PROT_WRITE);
	map_at(base + UNIT, 1, 0, PROT_READ | PROT_WRITE);
	ok = maps_file_pages(base, 1, 0, "first copy");
	ok = maps_file_pages(base + UNIT, 1, 0, "second copy") && ok;
	*count = p3s_maps_count(base, 2 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

/* 4K at file page 1 then 4K at file page 3: same folio, skipped slice. */
static bool case_phase_after(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	bool ok;

	map_at(base + UNIT, 1, 1, PROT_READ);
	map_at(base + 2 * UNIT, 1, 3, PROT_READ);
	ok = maps_file_pages(base + UNIT, 1, 1, "first mapping");
	ok = maps_file_pages(base + 2 * UNIT, 1, 3, "second mapping") && ok;
	*count = p3s_maps_count(base + UNIT, 2 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

/* mremap() a 4K mapping of file page 5 next to a 16K mapping of page 0. */
static bool case_mremap(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	unsigned char *src, *dst;
	bool ok;

	map_at(base, 4, 0, PROT_READ);
	src = mmap(NULL, UNIT, PROT_READ, MAP_SHARED, file_fd, 5 * UNIT);
	if (src == MAP_FAILED)
		ksft_exit_fail_msg("mmap source: %s\n", strerror(errno));
	dst = mremap(src, UNIT, UNIT, MREMAP_MAYMOVE | MREMAP_FIXED,
		     base + 4 * UNIT);
	if (dst == MAP_FAILED)
		ksft_exit_fail_msg("mremap: %s\n", strerror(errno));
	ok = maps_file_pages(base, 4, 0, "first mapping");
	ok = maps_file_pages(dst, 1, 5, "moved mapping") && ok;
	*count = p3s_maps_count(base, 5 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

/* Linear continuation must still work: page 0..3 then page 4..7. */
static bool case_linear(int *count)
{
	unsigned char *raw, *base = reserve(8 * UNIT, &raw);
	bool ok;

	map_at(base, 4, 0, PROT_READ);
	map_at(base + 4 * UNIT, 3, 4, PROT_READ);
	ok = maps_file_pages(base, 7, 0, "linear mappings");
	*count = p3s_maps_count(base, 7 * UNIT);
	munmap(raw, 8 * UNIT + 2 * NATIVE_PAGE_SIZE);
	return ok;
}

TEST_F(file_slice, fixed_after_keeps_skipped_file_page_separate)
{
	int count;

	ASSERT_TRUE(case_fixed_after(&count));
	EXPECT_EQ(2, count);
}

TEST_F(file_slice, fixed_before_keeps_noncontiguous_offset_separate)
{
	int count;

	ASSERT_TRUE(case_before(&count));
	EXPECT_EQ(2, count);
}

TEST_F(file_slice, mirror_keeps_repeated_offset_separate)
{
	int count;

	ASSERT_TRUE(case_mirror(&count));
	EXPECT_EQ(2, count);
}

TEST_F(file_slice, phase_after_keeps_skipped_slice_separate)
{
	int count;

	ASSERT_TRUE(case_phase_after(&count));
	EXPECT_EQ(2, count);
}

TEST_F(file_slice, fixed_mremap_keeps_skipped_offset_separate)
{
	int count;

	ASSERT_TRUE(case_mremap(&count));
	EXPECT_EQ(2, count);
}

TEST_F(file_slice, linear_continuation_merges)
{
	int count;

	ASSERT_TRUE(case_linear(&count));
	EXPECT_EQ(1, count);
}

P3S_HARNESS_COMPAT_MAIN
