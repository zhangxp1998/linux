// SPDX-License-Identifier: GPL-2.0
/*
 * Anonymous 4K tuples of a compat process survive holes: partial
 * MADV_DONTNEED, fork COW, mremap, mprotect and bulk discards keep the
 * surviving slices in one native folio and RssAnon in step with Anonymous.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#include "p3s_tuple_test.h"
#include "p3s_harness.h"

#define RATIO_TUPLES 1000
#define BULK_TUPLES 64

/* A compat PTE table has 512 entries, covering 2M of virtual addresses. */
#define COMPAT_PMD_SIZE		(512 * PROCESS_PAGE_SIZE)
#define SPARSE_ALIGNMENT		(4 * COMPAT_PMD_SIZE)
#define SPARSE_MREMAP_TIMEOUT_MS	10000


static bool fork_tuple_cow(unsigned char *base)
{
	int ready[2], done[2], status;
	char byte;
	uint64_t pfn[P3S_SLICES];
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
		_exit(check_tuple(base, 0x31) ? 0 : 1);
	}
	close(ready[1]);
	close(done[0]);
	passed = read(ready[0], &byte, 1) == 1;
	base[0] = 0x71;
	passed &= base[0] == 0x71 && read_pfns(base, pfn) && same_pfn(pfn);
	passed &= write(done[1], "D", 1) == 1;
	close(ready[0]);
	close(done[1]);
	passed &= waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		!WEXITSTATUS(status);
	base[0] = 0x31;
	return passed;
}

static bool mremap_tuple(bool shifted)
{
	unsigned char *src_reservation = NULL, *dst_reservation = NULL;
	unsigned char *src, *dst_area, *dst;
	uint64_t pfn[P3S_SLICES];
	void *moved;
	bool passed = false;

	src = map_aligned(NATIVE_PAGE_SIZE, &src_reservation);
	dst_area = map_aligned(3 * NATIVE_PAGE_SIZE, &dst_reservation);
	if (src == MAP_FAILED || dst_area == MAP_FAILED)
		goto out;
	dst = dst_area + (shifted ? PROCESS_PAGE_SIZE : 0);
	fill_tuple(src, 0x41);
	memset(dst, 0x69, NATIVE_PAGE_SIZE);
	errno = 0;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (shifted)
		passed = moved == MAP_FAILED && errno == EINVAL &&
			 check_tuple(src, 0x41) &&
			 all_bytes_are(dst, NATIVE_PAGE_SIZE, 0x69);
	else
		passed = moved == dst && check_tuple(dst, 0x41) &&
			 read_pfns(dst, pfn) && same_pfn(pfn);
out:
	if (src_reservation)
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
	if (dst_reservation)
		munmap(dst_reservation, 4 * NATIVE_PAGE_SIZE);
	return passed;
}

static bool mremap_zero_tuple_shifted(void)
{
	unsigned char *src_reservation = NULL, *dst_reservation = NULL;
	unsigned char *src, *dst_area, *dst;
	uint64_t pfn[P3S_SLICES];
	unsigned char value = 0;
	unsigned int slice;
	void *moved;
	bool passed = false;

	src = map_aligned(NATIVE_PAGE_SIZE, &src_reservation);
	dst_area = map_aligned(3 * NATIVE_PAGE_SIZE, &dst_reservation);
	if (src == MAP_FAILED || dst_area == MAP_FAILED)
		goto out;
	dst = dst_area + PROCESS_PAGE_SIZE;
	for (slice = 0; slice < P3S_SLICES; slice++)
		value |= src[slice * PROCESS_PAGE_SIZE];
	if (value)
		goto out;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (moved != MAP_FAILED || errno != EINVAL)
		goto out;
	dst = dst_area;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (moved != dst)
		goto out;
	for (slice = 0; slice < P3S_SLICES; slice++)
		if (dst[slice * PROCESS_PAGE_SIZE])
			goto out;
	fill_tuple(dst, 0x55);
	passed = check_tuple(dst, 0x55) && read_pfns(dst, pfn) &&
		 same_pfn(pfn);
out:
	if (src_reservation)
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
	if (dst_reservation)
		munmap(dst_reservation, 4 * NATIVE_PAGE_SIZE);
	return passed;
}

static bool mremap_dontunmap_slices(void)
{
	unsigned char *src_reservation = NULL, *dst_reservation = NULL;
	unsigned char *src, *dst_area;
	unsigned int round;
	bool passed = false;

	src = map_aligned(NATIVE_PAGE_SIZE, &src_reservation);
	dst_area = map_aligned(NATIVE_PAGE_SIZE, &dst_reservation);
	if (src == MAP_FAILED || dst_area == MAP_FAILED)
		goto out;

	for (round = 0; round < 128; round++) {
		unsigned int src_slice = round & (P3S_SLICES - 1);
		unsigned int dst_slice = (src_slice + 1) & (P3S_SLICES - 1);
		unsigned char *src_page = src + src_slice * PROCESS_PAGE_SIZE;
		unsigned char *dst_page = dst_area + dst_slice * PROCESS_PAGE_SIZE;
		unsigned char value = 0x41 + src_slice;
		void *moved;

		memset(src_page, value, PROCESS_PAGE_SIZE);
		moved = mremap(src_page, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
				MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
				dst_page);
		if (moved != MAP_FAILED || errno != EINVAL || src_page[0] != value)
			goto out;
		dst_page = dst_area + src_slice * PROCESS_PAGE_SIZE;
		moved = mremap(src_page, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			      MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
			      dst_page);
		if (moved != dst_page || dst_page[0] != value ||
		    dst_page[PROCESS_PAGE_SIZE - 1] != value || src_page[0] != 0)
			goto out;
	}
	passed = true;
out:
	if (src_reservation)
		munmap(src_reservation, 2 * NATIVE_PAGE_SIZE);
	if (dst_reservation)
		munmap(dst_reservation, 2 * NATIVE_PAGE_SIZE);
	return passed;
}

/* Run only in a child; exiting also releases the reservations and mappings. */
static bool mremap_sparse_range(bool dontunmap)
{
	const size_t len = 3 * COMPAT_PMD_SIZE;
	const size_t reserve_len = len + SPARSE_ALIGNMENT + 2 * NATIVE_PAGE_SIZE;
	unsigned char *src_reservation, *dst_reservation = NULL;
	unsigned char *src, *dst_area, *dst;
	int flags = MREMAP_MAYMOVE | MREMAP_FIXED;

	src_reservation = mmap(NULL, reserve_len, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src_reservation == MAP_FAILED) {
		ksft_perror("sparse mremap: reserve source");
		return false;
	}
	src = (unsigned char *)(((uintptr_t)src_reservation + NATIVE_PAGE_SIZE +
				SPARSE_ALIGNMENT - 1) & ~(SPARSE_ALIGNMENT - 1));
	if (mprotect(src, len, PROT_READ | PROT_WRITE)) {
		ksft_perror("sparse mremap: mprotect source");
		return false;
	}
	dst_area = map_exact_edge(len + NATIVE_PAGE_SIZE, &dst_reservation);
	if (dst_area == MAP_FAILED) {
		ksft_perror("sparse mremap: reserve destination");
		return false;
	}
	dst = dst_area;

	/*
	 * Keep all three PMDs within one compat PUD.  Faulting the end tuples
	 * installs the upper page tables, but the whole middle PMD is untouched:
	 * mm_find_pmd() must return an empty PMD slot, not NULL.  In particular,
	 * do not memset/prefault the range and then discard it with DONTNEED:
	 * that can leave an allocated PTE table and miss the original livelock.
	 * Keep the source phase; no page copying is needed to move the PTEs.
	 */
	fill_tuple(src, 0x41);
	fill_tuple(src + len - NATIVE_PAGE_SIZE, 0x61);
	if (dontunmap)
		flags |= MREMAP_DONTUNMAP;
	if (mremap(src, len, len, flags, dst) != dst) {
		ksft_perror("sparse mremap");
		return false;
	}
	if (!check_tuple(dst, 0x41) ||
	    !check_tuple(dst + len - NATIVE_PAGE_SIZE, 0x61) ||
	    !all_bytes_are(dst + NATIVE_PAGE_SIZE, len - 2 * NATIVE_PAGE_SIZE, 0)) {
		ksft_print_msg("sparse mremap: moved data or hole contents differ\n");
		return false;
	}

	/* The moved hole must accept writes, independently of the retained VMA. */
	fill_tuple(dst + COMPAT_PMD_SIZE, 0x71);
	if (dontunmap) {
		if (!all_bytes_are(src, len, 0)) {
			ksft_print_msg("sparse DONTUNMAP: source did not refault as zero\n");
			return false;
		}
		fill_tuple(src + COMPAT_PMD_SIZE, 0x81);
		if (!check_tuple(src + COMPAT_PMD_SIZE, 0x81))
			return false;
	}
	return check_tuple(dst + COMPAT_PMD_SIZE, 0x71);
}

static bool mremap_sparse_with_timeout(bool dontunmap)
{
	struct timespec start, now;
	pid_t child, waited;
	long elapsed_ms;
	int status;

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return false;
	fflush(stdout);
	child = fork();
	if (child < 0) {
		ksft_perror("sparse mremap: fork");
		return false;
	}
	if (!child) {
		bool passed = mremap_sparse_range(dontunmap);

		fflush(stdout);
		_exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	/* A thread in the child's mm could itself block on its mmap_lock. */
	for (;;) {
		waited = waitpid(child, &status, WNOHANG);
		if (waited == child) {
			if (WIFSIGNALED(status))
				ksft_print_msg("sparse mremap: child killed by signal %d\n",
					       WTERMSIG(status));
			return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
		}
		if (waited < 0 && errno != EINTR) {
			ksft_perror("sparse mremap: waitpid");
			return false;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			break;
		elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
			     (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed_ms >= SPARSE_MREMAP_TIMEOUT_MS) {
			ksft_print_msg("sparse mremap%s: timed out after %d ms\n",
				       dontunmap ? " DONTUNMAP" : "",
				       SPARSE_MREMAP_TIMEOUT_MS);
			break;
		}
		usleep(10000);
	}
	/* The old retry loop checks fatal_signal_pending(), so SIGKILL exits it. */
	kill(child, SIGKILL);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return false;
}

static bool incompatible_vma_edge(void)
{
	unsigned char *reservation = NULL, *base, *low, *high;
	uint64_t pfn[P3S_SLICES];
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
	if (!read_pfns(base, pfn) || !pfn[0] || pfn[0] != pfn[1] ||
	    !pfn[2] || pfn[2] != pfn[3] || pfn[0] == pfn[2])
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
	uint64_t pfn[P3S_SLICES];
	unsigned long anonymous = 0;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	fill_tuple(base, 0x31);
	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			      MADV_DONTNEED));
	EXPECT_EQ(0x31, base[0]);
	EXPECT_EQ(0x33, base[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x34, base[3 * PROCESS_PAGE_SIZE]);
	base[PROCESS_PAGE_SIZE] = 0x32;
	EXPECT_EQ(0x32, base[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0, base[PROCESS_PAGE_SIZE + 1]);
	EXPECT_EQ(0, base[2 * PROCESS_PAGE_SIZE - 1]);
	EXPECT_TRUE(same_present_pfns(base, remaining, 3));
	EXPECT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Anonymous",
				     &anonymous));
	EXPECT_EQ(NATIVE_PAGE_SIZE, anonymous);
	memset(base + PROCESS_PAGE_SIZE, 0x32, PROCESS_PAGE_SIZE);
	EXPECT_TRUE(check_tuple(base, 0x31));
	EXPECT_TRUE(read_pfns(base, pfn) && same_pfn(pfn));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_middle_fault_leaves_siblings_absent)
{
	unsigned char *reservation = NULL, *base;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	memset(base + 2 * PROCESS_PAGE_SIZE, 0x52, PROCESS_PAGE_SIZE);
	EXPECT_TRUE(only_slice_present(base, 2));
	EXPECT_EQ(0, base[0]);
	EXPECT_EQ(0, base[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x52, base[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0, base[3 * PROCESS_PAGE_SIZE]);
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_8k_edge_uses_one_folio)
{
	unsigned char *reservation = NULL, *base;

	base = map_exact_edge(2 * PROCESS_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	memset(base, 0x61, PROCESS_PAGE_SIZE);
	memset(base + PROCESS_PAGE_SIZE, 0x62, PROCESS_PAGE_SIZE);
	EXPECT_TRUE(same_present_pfns(base,
			(const unsigned int[]){ 0, 1 }, 2));
	munmap(reservation, 2 * PROCESS_PAGE_SIZE + 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_fork_cows_one_tuple)
{
	unsigned char *reservation = NULL, *base;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	fill_tuple(base, 0x31);
	EXPECT_TRUE(fork_tuple_cow(base));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_aligned_mremap)
{
	EXPECT_TRUE(mremap_tuple(false));
}

TEST(hole_anon_cross_phase_mremap_rejected)
{
	EXPECT_TRUE(mremap_tuple(true));
}

TEST(hole_anon_zero_tuple_mremap)
{
	EXPECT_TRUE(mremap_zero_tuple_shifted());
}

TEST(hole_anon_dontunmap_mremap)
{
	EXPECT_TRUE(mremap_dontunmap_slices());
}

TEST(hole_anon_sparse_mremap_across_empty_pmd)
{
	EXPECT_TRUE(mremap_sparse_with_timeout(false));
}

TEST(hole_anon_sparse_dontunmap_across_empty_pmd)
{
	EXPECT_TRUE(mremap_sparse_with_timeout(true));
}

TEST(hole_anon_unrelated_vma_edge_reclaimable)
{
	EXPECT_TRUE(incompatible_vma_edge());
}

TEST(hole_anon_partial_mprotect_keeps_tuple)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES];

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	fill_tuple(base, 0x31);
	EXPECT_EQ(0, mprotect(base + PROCESS_PAGE_SIZE,
			      PROCESS_PAGE_SIZE, PROT_READ));
	EXPECT_TRUE(read_pfns(base, pfn) && same_pfn(pfn));
	EXPECT_EQ(0, mprotect(base + PROCESS_PAGE_SIZE,
			      PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE));
	EXPECT_TRUE(check_tuple(base, 0x31));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_sparse_holes_keep_rss_bounded)
{
	unsigned char *reservation = NULL, *base;
	unsigned long anonymous = 0;
	long rss_before, rss_after;
	double ratio;

	rss_anon_bytes(); /* Warm the parsers before taking the delta. */
	base = map_aligned(RATIO_TUPLES * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	rss_before = rss_anon_bytes();
	for (unsigned int i = 0; i < RATIO_TUPLES; i++) {
		fill_tuple(base + i * NATIVE_PAGE_SIZE, 0x31);
		ASSERT_EQ(0, madvise(base + i * NATIVE_PAGE_SIZE +
				(i & (P3S_SLICES - 1)) * PROCESS_PAGE_SIZE,
				PROCESS_PAGE_SIZE, MADV_DONTNEED));
	}
	rss_after = rss_anon_bytes();
	ASSERT_GE(rss_before, 0);
	ASSERT_GE(rss_after, rss_before);
	ASSERT_TRUE(p3s_smaps_bytes(base, RATIO_TUPLES * NATIVE_PAGE_SIZE,
				    "Anonymous", &anonymous));
	ASSERT_NE(anonymous, 0);
	ratio = (double)(rss_after - rss_before) / anonymous;
	TH_LOG("RssAnon/Anonymous=%.3f", ratio);
	EXPECT_LE(ratio, 1.34);
	munmap(reservation, (RATIO_TUPLES + 1) * NATIVE_PAGE_SIZE);
}

TEST(hole_anon_bulk_dontneed_leaves_end_slices)
{
	unsigned char *reservation = NULL, *base;
	unsigned long anonymous = 0;
	long rss_before, rss_after;

	base = map_aligned(BULK_TUPLES * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	rss_before = rss_anon_bytes();
	for (unsigned int i = 0; i < BULK_TUPLES; i++)
		fill_tuple(base + i * NATIVE_PAGE_SIZE, 0x31);
	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE,
			    BULK_TUPLES * NATIVE_PAGE_SIZE - 2 * PROCESS_PAGE_SIZE,
			    MADV_DONTNEED));
	rss_after = rss_anon_bytes();
	ASSERT_GE(rss_before, 0);
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE, rss_after - rss_before);
	ASSERT_TRUE(p3s_smaps_bytes(base, BULK_TUPLES * NATIVE_PAGE_SIZE,
				    "Anonymous", &anonymous));
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE, anonymous);
	EXPECT_EQ(0x31, base[0]);
	EXPECT_EQ(0x34, base[BULK_TUPLES * NATIVE_PAGE_SIZE - 1]);
	munmap(reservation, (BULK_TUPLES + 1) * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_COMPAT_MAIN
