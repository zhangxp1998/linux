// SPDX-License-Identifier: GPL-2.0
/* Anonymous 4KB slices share a folio across faults, fork and pageout. */
#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/wait.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define DEPACK_RACE_ATTEMPTS 1000

static void populate(unsigned char *base)
{
	fill_slices(base, 0x31);
}

static bool verify(const unsigned char *base)
{
	return check_slices(base, 0x31);
}

static bool adjacent_anon_mmaps_merge(void)
{
	unsigned char *reservation;
	unsigned char *base;
	unsigned long start, end, lo, hi;
	char line[256];
	FILE *maps;
	unsigned int i, overlaps = 0;

	base = map_aligned_prot(NATIVE_PAGE_SIZE, PROT_NONE, &reservation);
	if (base == MAP_FAILED)
		return false;
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);

	for (i = 0; i < P3S_SLICES; i++) {
		void *mapped = mmap(base + i * PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS |
				    MAP_FIXED_NOREPLACE, -1, 0);

		if (mapped == MAP_FAILED)
			goto out;
	}

	start = (unsigned long)base;
	end = start + NATIVE_PAGE_SIZE;
	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		goto out;
	while (fgets(line, sizeof(line), maps))
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
		    lo < end && hi > start)
			overlaps++;
	fclose(maps);
out:
	munmap(base, NATIVE_PAGE_SIZE);
	return overlaps == 1;
}

static bool packed_smaps_matches(const unsigned char *base,
				 unsigned long pss,
				 unsigned long shared_dirty,
				 unsigned long private_dirty)
{
	unsigned long value;

	return p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Rss", &value) &&
		value == NATIVE_PAGE_SIZE &&
		p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Pss", &value) &&
		value == pss &&
		p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Pss_Dirty", &value) &&
		value == pss &&
		p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Shared_Dirty", &value) &&
		value == shared_dirty &&
		p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Private_Dirty", &value) &&
		value == private_dirty;
}

static bool fork_smaps_shares_page(unsigned char *base)
{
	int child_ready[2];
	int child_done[2];
	char byte;
	pid_t child;
	int status;
	bool passed;

	if (pipe(child_ready))
		return false;
	if (pipe(child_done)) {
		close(child_ready[0]);
		close(child_ready[1]);
		return false;
	}
	child = fork();
	if (child < 0) {
		close(child_ready[0]);
		close(child_ready[1]);
		close(child_done[0]);
		close(child_done[1]);
		return false;
	}
	if (!child) {
		close(child_ready[0]);
		close(child_done[1]);
		if (write(child_ready[1], "R", 1) != 1 ||
		    read(child_done[0], &byte, 1) != 1)
			_exit(1);
		_exit(verify(base) ? 0 : 1);
	}

	close(child_ready[1]);
	close(child_done[0]);
	passed = read(child_ready[0], &byte, 1) == 1 &&
		 packed_smaps_matches(base, NATIVE_PAGE_SIZE / 2, NATIVE_PAGE_SIZE, 0);
	if (write(child_done[1], "D", 1) != 1)
		passed = false;
	close(child_ready[0]);
	close(child_done[1]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		passed = false;
	return passed;
}

static bool fork_cow_preserves_parent(unsigned char *base)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		base[PROCESS_PAGE_SIZE] = 0xa5;
		if (base[PROCESS_PAGE_SIZE] != 0xa5 ||
		    base[0] != 0x31 || base[2 * PROCESS_PAGE_SIZE] != 0x33 ||
		    base[3 * PROCESS_PAGE_SIZE] != 0x34)
			_exit(1);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child)
		return false;
	return WIFEXITED(status) && !WEXITSTATUS(status) && verify(base);
}

struct concurrent_write_args {
	unsigned char *base;
	unsigned int slice;
	atomic_bool *start;
};

static void *write_zero_page_slice(void *data)
{
	struct concurrent_write_args *args = data;

	while (!atomic_load_explicit(args->start, memory_order_acquire))
		sched_yield();
	memset(args->base + args->slice * PROCESS_PAGE_SIZE, 0x31 + args->slice,
	       PROCESS_PAGE_SIZE);
	return NULL;
}

static bool concurrent_zero_page_promotion(unsigned char *base)
{
	struct concurrent_write_args args[P3S_SLICES];
	pthread_t threads[P3S_SLICES];
	atomic_bool start = false;
	unsigned char value;
	unsigned int created = 0;
	bool passed = true;

	value = base[2 * PROCESS_PAGE_SIZE];
	if (value)
		return false;

	for (created = 0; created < P3S_SLICES; created++) {
		args[created].base = base;
		args[created].slice = created;
		args[created].start = &start;
		if (pthread_create(&threads[created], NULL,
				   write_zero_page_slice, &args[created])) {
			passed = false;
			break;
		}
	}
	atomic_store_explicit(&start, true, memory_order_release);
	while (created)
		passed &= !pthread_join(threads[--created], NULL);

	return passed && verify(base);
}

struct depack_race_args {
	unsigned char *base;
	atomic_bool ready;
	atomic_bool stop;
};

static void *write_during_depack(void *data)
{
	struct depack_race_args *args = data;
	atomic_uchar *target = (atomic_uchar *)args->base;
	unsigned char value = 0;

	atomic_store_explicit(&args->ready, true, memory_order_release);
	while (!atomic_load_explicit(&args->stop, memory_order_acquire))
		atomic_store_explicit(target, ++value, memory_order_relaxed);
	return NULL;
}

static bool concurrent_partial_madvise(void)
{
	unsigned int attempt;

	for (attempt = 0; attempt < DEPACK_RACE_ATTEMPTS; attempt++) {
		struct depack_race_args args;
		unsigned char *reservation;
		unsigned char *base;
		pthread_t thread;
		bool passed;
		int error;
		int ret;

		base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
		if (base == MAP_FAILED)
			return false;
		populate(base);
		args.base = base;
		atomic_init(&args.ready, false);
		atomic_init(&args.stop, false);
		ret = pthread_create(&thread, NULL, write_during_depack, &args);
		if (ret) {
			munmap(reservation, 3 * NATIVE_PAGE_SIZE);
			return false;
		}
		while (!atomic_load_explicit(&args.ready, memory_order_acquire))
			sched_yield();
		for (ret = 0; ret < 64; ret++)
			sched_yield();

		errno = 0;
		ret = madvise(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			      MADV_DONTNEED);
		error = errno;
		atomic_store_explicit(&args.stop, true, memory_order_release);
		passed = !pthread_join(thread, NULL) && !ret &&
			 base[PROCESS_PAGE_SIZE] == 0 &&
			 base[2 * PROCESS_PAGE_SIZE] == 0x33 &&
			 base[3 * PROCESS_PAGE_SIZE] == 0x34;
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		if (!passed) {
			errno = error;
			return false;
		}
	}
	return true;
}

TEST(packed_anon_adjacent_mmaps_merge)
{
	EXPECT_TRUE(adjacent_anon_mmaps_merge());
}

TEST(packed_anon_page_rss_and_pss)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES];
	unsigned long rss_before, rss_after;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	ASSERT_TRUE(verify(base));
	ASSERT_EQ(0, madvise(base, NATIVE_PAGE_SIZE, MADV_DONTNEED));
	ASSERT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Rss",
				    &rss_before));
	populate(base);
	ASSERT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Rss",
				    &rss_after));
	EXPECT_EQ(NATIVE_PAGE_SIZE, rss_after - rss_before);
	EXPECT_TRUE(packed_smaps_matches(base, NATIVE_PAGE_SIZE, 0,
					 NATIVE_PAGE_SIZE));
	ASSERT_TRUE(read_pfns(base, pfn));
	if (!pfn[0]) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_smaps_shares_page_across_fork)
{
	unsigned char *reservation = NULL, *base;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	EXPECT_TRUE(fork_smaps_shares_page(base));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_fork_cow_preserves_parent)
{
	unsigned char *reservation = NULL, *base;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	EXPECT_TRUE(fork_cow_preserves_parent(base));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_partial_mprotect_preserves_folio)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES];

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	ASSERT_EQ(0, mprotect(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			      PROT_READ));
	EXPECT_TRUE(verify(base));
	ASSERT_TRUE(read_pfns(base, pfn));
	EXPECT_EQ(0, mprotect(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			      PROT_READ | PROT_WRITE));
	if (!pfn[0]) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_mlock_preserves_folio)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES];

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	ASSERT_EQ(0, mlock(base, NATIVE_PAGE_SIZE));
	EXPECT_TRUE(verify(base));
	ASSERT_TRUE(read_pfns(base, pfn));
	EXPECT_EQ(0, munlock(base, NATIVE_PAGE_SIZE));
	if (!pfn[0]) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_concurrent_zero_page_promotion)
{
	unsigned char *reservation = NULL, *base;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	EXPECT_TRUE(concurrent_zero_page_promotion(base));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_pageout_restores_page_slices)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES], before[P3S_SLICES];
	unsigned long rss, swap;

	if (!p3s_swap_available())
		SKIP(return, "no active swap");
	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	populate(base);
	ASSERT_TRUE(read_pfns(base, before));
	if (before[0])
		ASSERT_TRUE(same_pfn(before));
	ASSERT_TRUE(pageout_reaches_swap(base, NATIVE_PAGE_SIZE,
					 NATIVE_PAGE_SIZE));
	EXPECT_TRUE(verify(base));
	ASSERT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Rss", &rss));
	EXPECT_EQ(NATIVE_PAGE_SIZE, rss);
	ASSERT_TRUE(p3s_smaps_bytes(base, NATIVE_PAGE_SIZE, "Swap", &swap));
	EXPECT_EQ(0, swap);
	ASSERT_TRUE(read_pfns(base, pfn));
	if (!before[0] || !pfn[0]) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(packed_anon_concurrent_partial_dontneed)
{
	EXPECT_TRUE(concurrent_partial_madvise());
}

TEST(packed_anon_zero_page_promotion)
{
	unsigned char *reservation = NULL, *base;
	uint64_t pfn[P3S_SLICES];
	unsigned char value;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(base, MAP_FAILED);
	value = base[2 * PROCESS_PAGE_SIZE];
	EXPECT_EQ(0, value);
	EXPECT_TRUE(only_slice_present(base, 2));
	populate(base);
	EXPECT_TRUE(verify(base));
	ASSERT_TRUE(read_pfns(base, pfn));
	if (!pfn[0]) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
