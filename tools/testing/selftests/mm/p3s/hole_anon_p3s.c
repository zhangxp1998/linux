// SPDX-License-Identifier: GPL-2.0
/* Anonymous slice holes preserve neighbouring data and exact RSS accounting. */
#define _GNU_SOURCE

#include <sys/wait.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define RATIO_PAGES 1000
#define BULK_PAGES 64

static bool fork_page_cow(unsigned char *base, bool *pfns_hidden)
{
	int ready[2], done[2], status;
	char byte;
	uint64_t pfn[P3S_SLICES] = {};
	pid_t child;
	bool passed;

	if (pipe(ready) || pipe(done))
		return false;
	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (write(ready[1], "R", 1) != 1 ||
		    read(done[0], &byte, 1) != 1)
			_exit(1);
		_exit(check_slices(base, 0x31) ? 0 : 1);
	}
	close(ready[1]);
	close(done[0]);
	passed = read(ready[0], &byte, 1) == 1;
	base[0] = 0x71;
	passed &= base[0] == 0x71;
	if (read_pfns(base, pfn)) {
		*pfns_hidden = !pfn[0];
		if (!*pfns_hidden)
			passed &= same_pfn(pfn);
	} else {
		passed = false;
	}
	passed &= write(done[1], "D", 1) == 1;
	close(ready[0]);
	close(done[1]);
	passed &= waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		!WEXITSTATUS(status);
	base[0] = 0x31;
	return passed;
}

/* Adjacent incompatible VMAs in one 16KB window use distinct folios. */
static bool incompatible_vma_edge(bool *pfns_hidden)
{
	unsigned char *reservation = NULL, *base, *low, *high;
	uint64_t pfn[P3S_SLICES] = {};
	bool passed = false;

	base = map_exact_edge(NATIVE_PAGE_SIZE, &reservation);
	if (base == MAP_FAILED)
		return false;
	low = mmap(base, 2 * PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	high = mmap(base + 2 * PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
		    -1, 0);
	if (low != base || high != base + 2 * PROCESS_PAGE_SIZE)
		goto out;
	memset(low, 0x71, 2 * PROCESS_PAGE_SIZE);
	memset(high, 0x72, 2 * PROCESS_PAGE_SIZE);
	if (!read_pfns(base, pfn))
		goto out;
	*pfns_hidden = !pfn[0] || !pfn[2];
	if (!*pfns_hidden &&
	    (pfn[0] != pfn[1] || pfn[2] != pfn[3] || pfn[0] == pfn[2]))
		goto out;
	if (munmap(low, 2 * PROCESS_PAGE_SIZE))
		goto out;
	passed = !madvise(high, 2 * PROCESS_PAGE_SIZE, MADV_PAGEOUT) &&
		high[0] == 0x72 && high[2 * PROCESS_PAGE_SIZE - 1] == 0x72;
out:
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
	return passed;
}

TEST(hole_anon_partial_dontneed_refault)
{
	const unsigned int remaining[] = { 0, 2, 3 };
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES], probe;
	unsigned long anonymous = 0;
	bool have_pfns;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	fill_slices(base, 0x31);
	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	EXPECT_EQ(0x31, base[0]);
	EXPECT_EQ(0x33, base[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x34, base[3 * PROCESS_PAGE_SIZE]);
	base[PROCESS_PAGE_SIZE] = 0x32;
	EXPECT_EQ(0x32, base[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0, base[PROCESS_PAGE_SIZE + 1]);
	EXPECT_EQ(0, base[2 * PROCESS_PAGE_SIZE - 1]);
	have_pfns = p3s_pfn(base, &probe);
	if (have_pfns)
		EXPECT_TRUE(same_present_pfns(base, remaining, 3));
	EXPECT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Anonymous",
				    &anonymous));
	EXPECT_EQ(NATIVE_PAGE_SIZE, anonymous);
	memset(base + PROCESS_PAGE_SIZE, 0x32, PROCESS_PAGE_SIZE);
	EXPECT_TRUE(check_slices(base, 0x31));
	if (!have_pfns) {
		munmap(reservation, 2 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(read_pfns(base, pfn) && same_pfn(pfn));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_8k_edge_uses_one_folio)
{
	unsigned char *reservation = NULL, *base;
	uint64_t probe;

	base = map_exact_edge(2 * PROCESS_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	memset(base, 0x61, PROCESS_PAGE_SIZE);
	memset(base + PROCESS_PAGE_SIZE, 0x62, PROCESS_PAGE_SIZE);
	EXPECT_EQ(0x61, base[0]);
	EXPECT_EQ(0x62, base[PROCESS_PAGE_SIZE]);
	if (!p3s_pfn(base, &probe)) {
		munmap(reservation, 2 * PROCESS_PAGE_SIZE + 2 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_present_pfns(base,
				      (const unsigned int[]){ 0, 1 }, 2));
	munmap(reservation, 2 * PROCESS_PAGE_SIZE + 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_fork_cows_one_page)
{
	unsigned char *reservation = NULL, *base;
	bool pfns_hidden = false;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	fill_slices(base, 0x31);
	EXPECT_TRUE(fork_page_cow(base, &pfns_hidden));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
	if (pfns_hidden)
		SKIP(return, "PFNs unavailable");
}

TEST(hole_anon_unrelated_vma_edge_reclaimable)
{
	bool pfns_hidden = false;

	EXPECT_TRUE(incompatible_vma_edge(&pfns_hidden));
	if (pfns_hidden)
		SKIP(return, "PFNs unavailable");
}

TEST(hole_anon_sparse_holes_keep_rss_bounded)
{
	unsigned char *reservation = NULL, *base;
	unsigned long anonymous = 0;
	long rss_before, rss_after;

	rss_anon_bytes(); /* Warm the parsers before taking the delta. */
	base = map_aligned(RATIO_PAGES * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	rss_before = rss_anon_bytes();
	for (unsigned int i = 0; i < RATIO_PAGES; i++) {
		fill_slices(base + i * NATIVE_PAGE_SIZE, 0x31);
		ASSERT_EQ(0, madvise(base + i * NATIVE_PAGE_SIZE +
				(i & (P3S_SLICES - 1)) * PROCESS_PAGE_SIZE,
				PROCESS_PAGE_SIZE, MADV_DONTNEED));
	}
	rss_after = rss_anon_bytes();
	ASSERT_GE(rss_before, 0);
	ASSERT_GE(rss_after, rss_before);
	ASSERT_TRUE(p3s_smaps_bytes(base, RATIO_PAGES * NATIVE_PAGE_SIZE,
				    "Anonymous", &anonymous));
	ASSERT_NE(anonymous, 0);
	EXPECT_EQ(anonymous, rss_after - rss_before);
	munmap(reservation, (RATIO_PAGES + 1) * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_bulk_dontneed_leaves_end_slices)
{
	unsigned char *reservation = NULL, *base;
	unsigned long anonymous = 0;
	long rss_before, rss_after;

	base = map_aligned(BULK_PAGES * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	rss_before = rss_anon_bytes();
	for (unsigned int i = 0; i < BULK_PAGES; i++)
		fill_slices(base + i * NATIVE_PAGE_SIZE, 0x31);
	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE,
			     BULK_PAGES * NATIVE_PAGE_SIZE - 2 * PROCESS_PAGE_SIZE,
			    MADV_DONTNEED));
	rss_after = rss_anon_bytes();
	ASSERT_GE(rss_before, 0);
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE, rss_after - rss_before);
	ASSERT_TRUE(p3s_smaps_bytes(base, BULK_PAGES * NATIVE_PAGE_SIZE,
				    "Anonymous", &anonymous));
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE, anonymous);
	EXPECT_EQ(0x31, base[0]);
	EXPECT_EQ(0x34, base[BULK_PAGES * NATIVE_PAGE_SIZE - 1]);
	munmap(reservation, (BULK_PAGES + 1) * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
