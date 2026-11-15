// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

struct counts {
	unsigned long swap;
	unsigned long swap_pss;
};

static bool read_counts(void *start, size_t len, struct counts *out)
{
	unsigned long swap_bytes = 0, pss_bytes = 0;

	if (!p3s_smaps_bytes(start, len, "Swap", &swap_bytes) ||
	    !p3s_smaps_bytes(start, len, "SwapPss", &pss_bytes))
		return false;
	out->swap = swap_bytes / 1024;
	out->swap_pss = pss_bytes / 1024;
	return true;
}

static unsigned char *alloc_tuple(unsigned char **area_out)
{
	unsigned char *page;
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned long i;

	*area_out = MAP_FAILED;
	page = map_exact_edge(NATIVE_PAGE_SIZE, area_out);
	if (page == MAP_FAILED)
		return MAP_FAILED;
	/* Compat VMAs reject THP advice because they are non-THP. */
	if (madvise(page, NATIVE_PAGE_SIZE, MADV_NOHUGEPAGE) &&
	    errno != EINVAL) {
		munmap(*area_out, 3 * NATIVE_PAGE_SIZE);
		*area_out = MAP_FAILED;
		return MAP_FAILED;
	}
	for (i = 0; i < NATIVE_PAGE_SIZE; i++)
		page[i] = 1 + i / page_size;
	return page;
}

static int swap_and_check_pss(void *page, size_t len,
			      unsigned long expected_pss)
{
	struct counts counts;

	if (!pageout_reaches_swap(page, len, len))
		return KSFT_SKIP;
	if (!read_counts(page, NATIVE_PAGE_SIZE, &counts))
		return KSFT_FAIL;
	if (counts.swap_pss != expected_pss) {
		ksft_print_msg("Swap=%lu kB SwapPss=%lu kB, expected %lu kB\n",
			       counts.swap, counts.swap_pss, expected_pss);
		return KSFT_FAIL;
	}
	return KSFT_PASS;
}

static int test_exclusive(void)
{
	unsigned char *area, *page = alloc_tuple(&area);
	unsigned long i;
	int ret;

	if (page == MAP_FAILED)
		return KSFT_FAIL;
	ksft_print_msg("exclusive\n");
	ret = swap_and_check_pss(page, NATIVE_PAGE_SIZE, 16);
	if (ret != KSFT_PASS)
		goto out;
	for (i = 0; i < NATIVE_PAGE_SIZE; i++) {
		if (page[i] != 1 + i / PROCESS_PAGE_SIZE) {
			ksft_print_msg("swap-in data mismatch at %lu\n", i);
			ret = KSFT_FAIL;
			break;
		}
	}
out:
	munmap(area, 3 * NATIVE_PAGE_SIZE);
	return ret;
}

static int test_partial_unmap(void)
{
	unsigned char *area, *page = alloc_tuple(&area);
	struct counts counts;
	int ret;

	if (page == MAP_FAILED)
		return KSFT_FAIL;
	ksft_print_msg("partial-unmap\n");
	/* Drop one swap reference without faulting the remaining slices in. */
	ret = swap_and_check_pss(page, NATIVE_PAGE_SIZE, 16);
	if (ret != KSFT_PASS)
		goto out;
	if (munmap(page + 3 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE) ||
	    !read_counts(page, NATIVE_PAGE_SIZE, &counts)) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (counts.swap != 12 || counts.swap_pss != 12) {
		ksft_print_msg("partial-unmap: Swap=%lu kB SwapPss=%lu kB, expected 12 kB\n",
			       counts.swap, counts.swap_pss);
		ret = KSFT_FAIL;
	}
out:
	munmap(area, 3 * NATIVE_PAGE_SIZE);
	return ret;
}

static int test_fork(void)
{
	unsigned char *area, *page = alloc_tuple(&area);
	struct counts counts;
	int ready[2] = { -1, -1 }, done[2] = { -1, -1 }, status, ret;
	char byte;
	pid_t child = -1;

	if (page == MAP_FAILED)
		return KSFT_FAIL;
	ksft_print_msg("fork-shared\n");
	ret = swap_and_check_pss(page, NATIVE_PAGE_SIZE, 16);
	if (ret != KSFT_PASS)
		goto out;
	if (pipe(ready) || pipe(done)) {
		ret = KSFT_FAIL;
		goto out;
	}
	child = fork();
	if (child < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (write(ready[1], "r", 1) != 1 || read(done[0], &byte, 1) != 1)
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}
	close(ready[1]);
	ready[1] = -1;
	close(done[0]);
	done[0] = -1;
	if (read(ready[0], &byte, 1) != 1 ||
	    !read_counts(page, NATIVE_PAGE_SIZE, &counts)) {
		ret = KSFT_FAIL;
		goto out_child;
	}
	if (counts.swap != 16 || counts.swap_pss != 8) {
		ksft_print_msg("fork-shared: Swap=%lu kB SwapPss=%lu kB\n",
			       counts.swap, counts.swap_pss);
		ret = KSFT_FAIL;
	}
	if (write(done[1], "d", 1) != 1 ||
	    waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status)) {
		child = -1;
		ret = KSFT_FAIL;
		goto out;
	}
	child = -1;
	if (!read_counts(page, NATIVE_PAGE_SIZE, &counts) ||
	    counts.swap != 16 || counts.swap_pss != 16) {
		ksft_print_msg("fork-cleanup: Swap=%lu kB SwapPss=%lu kB\n",
			       counts.swap, counts.swap_pss);
		ret = KSFT_FAIL;
	}
out_child:
	if (child > 0) {
		close(done[1]);
		done[1] = -1;
		waitpid(child, &status, 0);
	}
out:
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	if (done[0] >= 0)
		close(done[0]);
	if (done[1] >= 0)
		close(done[1]);
	munmap(area, 3 * NATIVE_PAGE_SIZE);
	return ret;
}

static int test_split_vma_partial_swapin(void)
{
	unsigned char *area, *page = alloc_tuple(&area);
	struct counts swapped;
	int ret;

	if (page == MAP_FAILED)
		return KSFT_FAIL;
	ksft_print_msg("split-vma-partial-swapin\n");
	ret = swap_and_check_pss(page, NATIVE_PAGE_SIZE, 16);
	if (ret != KSFT_PASS)
		goto out;
	if (mprotect(page + NATIVE_PAGE_SIZE / 2, NATIVE_PAGE_SIZE / 2,
		     PROT_READ)) {
		ret = KSFT_FAIL;
		goto out;
	}

	/* Swap in only the first VMA; the second VMA keeps two swap PTEs. */
	if (page[0] != 1 ||
	    !read_counts(page + NATIVE_PAGE_SIZE / 2, NATIVE_PAGE_SIZE / 2,
			 &swapped)) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (swapped.swap != 8 || swapped.swap_pss != 8) {
		ksft_print_msg("split-vma-partial-swapin: Swap=%lu kB SwapPss=%lu kB\n",
			       swapped.swap, swapped.swap_pss);
		ret = KSFT_FAIL;
	}
out:
	munmap(area, 3 * NATIVE_PAGE_SIZE);
	return ret;
}

/* Fork while swapped, then retain the same partial window in both mms. */
static int test_partial_fork(unsigned int nr)
{
	unsigned char *area, *page = alloc_tuple(&area);
	struct counts counts = {}, child_counts = {};
	int ready[2] = { -1, -1 }, done[2] = { -1, -1 }, status, ret;
	unsigned long i;
	uint64_t per_pte = ((uint64_t)NATIVE_PAGE_SIZE << 12) / (2 * nr);
	unsigned long expected_pss =
		(per_pte < (4096ULL << 12) ? per_pte : 4096ULL << 12) *
		nr >> 22;
	char byte;
	pid_t child = -1;

	if (page == MAP_FAILED)
		return KSFT_FAIL;
	ksft_print_msg("partial-fork-%u-slices\n", nr);
	ret = swap_and_check_pss(page, NATIVE_PAGE_SIZE, 16);
	if (ret != KSFT_PASS)
		goto out;
	if (pipe(ready) || pipe(done)) {
		ret = KSFT_FAIL;
		goto out;
	}
	child = fork();
	if (child < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (munmap(page + nr * PROCESS_PAGE_SIZE,
			   (P3S_SLICES - nr) * PROCESS_PAGE_SIZE) ||
		    write(ready[1], "r", 1) != 1 ||
		    read(done[0], &byte, 1) != 1)
			_exit(EXIT_FAILURE);
		if (!read_counts(page, nr * PROCESS_PAGE_SIZE, &child_counts) ||
		    write(ready[1], &child_counts, sizeof(child_counts)) !=
		    sizeof(child_counts) ||
		    read(done[0], &byte, 1) != 1)
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}
	close(ready[1]);
	ready[1] = -1;
	close(done[0]);
	done[0] = -1;
	if (read(ready[0], &byte, 1) != 1 ||
	    munmap(page + nr * PROCESS_PAGE_SIZE,
		   (P3S_SLICES - nr) * PROCESS_PAGE_SIZE) ||
	    write(done[1], "r", 1) != 1 ||
	    read(ready[0], &child_counts, sizeof(child_counts)) !=
	    sizeof(child_counts) ||
	    !read_counts(page, nr * PROCESS_PAGE_SIZE, &counts)) {
		ret = KSFT_FAIL;
		goto out_child;
	}
	ksft_print_msg("partial-fork-%u: parent Swap=%lu SwapPss=%lu; child Swap=%lu SwapPss=%lu; expected Swap=%u SwapPss=%u kB\n",
		       nr, counts.swap, counts.swap_pss, child_counts.swap,
		       child_counts.swap_pss, 4 * nr,
		       (unsigned int)expected_pss);
	if (counts.swap != 4 * nr || counts.swap_pss != expected_pss ||
	    child_counts.swap != 4 * nr ||
	    child_counts.swap_pss != expected_pss)
		ret = KSFT_FAIL;
	if (write(done[1], "d", 1) != 1 ||
	    waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status)) {
		child = -1;
		ret = KSFT_FAIL;
		goto out;
	}
	child = -1;
	if (!read_counts(page, nr * PROCESS_PAGE_SIZE, &counts) ||
	    counts.swap != 4 * nr || counts.swap_pss != 4 * nr) {
		ksft_print_msg("partial-fork-%u cleanup: Swap=%lu SwapPss=%lu\n",
			       nr, counts.swap, counts.swap_pss);
		ret = KSFT_FAIL;
	}
	for (i = 0; i < nr * PROCESS_PAGE_SIZE; i++) {
		if (page[i] != 1 + i / PROCESS_PAGE_SIZE) {
			ksft_print_msg("partial-fork-%u data mismatch at %lu\n",
				       nr, i);
			ret = KSFT_FAIL;
			break;
		}
	}
out_child:
	if (child > 0) {
		close(done[1]);
		done[1] = -1;
		waitpid(child, &status, 0);
	}
out:
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	if (done[0] >= 0)
		close(done[0]);
	if (done[1] >= 0)
		close(done[1]);
	munmap(area, 3 * NATIVE_PAGE_SIZE);
	return ret;
}

#define CHECK_SWAP(case_expr) do { \
	if (!p3s_swap_available()) \
		SKIP(return, "no active swap device"); \
	int ret = (case_expr); \
	if (ret == KSFT_SKIP) \
		SKIP(return, "swapout did not establish the test precondition"); \
	EXPECT_EQ(KSFT_PASS, ret); \
} while (0)

TEST(exclusive_swap_pss_and_data)
{
	CHECK_SWAP(test_exclusive());
}

TEST(partial_unmap_updates_swap_pss)
{
	CHECK_SWAP(test_partial_unmap());
}

TEST(split_vma_keeps_remaining_swap_pss)
{
	CHECK_SWAP(test_split_vma_partial_swapin());
}

TEST(fork_halves_shared_swap_pss)
{
	CHECK_SWAP(test_fork());
}

TEST(partial_fork_one_slice)
{
	CHECK_SWAP(test_partial_fork(1));
}

TEST(partial_fork_two_slices)
{
	CHECK_SWAP(test_partial_fork(2));
}

TEST(partial_fork_three_slices)
{
	CHECK_SWAP(test_partial_fork(3));
}

P3S_HARNESS_4KB_MAIN
