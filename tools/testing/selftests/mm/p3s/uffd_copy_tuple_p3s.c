// SPDX-License-Identifier: GPL-2.0
/*
 * P3S: UFFDIO_COPY must install whole native tuples, not singletons.
 *
 * A compat (4K) process runs on 4K page tables while the kernel's native
 * PAGE_SIZE is 16K, so every anonymous PTE is backed by a slice of a native
 * folio.  mfill_atomic() walks the request in MM_PAGE_SIZE(mm) (4K) steps and
 * allocates one native folio per step, so a UFFDIO_COPY-installed page used to
 * become a "singleton": a 16K folio carrying exactly one 4K PTE.  Multi-page
 * UFFDIO_COPY calls carry most of the bytes installed by ART's userfaultfd GC,
 * so that inflated the collector's anonymous footprint 4x.
 *
 * K1 ("tuple-aware mfill") adds a fast path: when a private anonymous COPY is
 * 16K-aligned and at least 16K long, one folio is copied whole and mapped
 * behind all four compat PTEs under a single pte lock.
 *
 * What this test asserts, from a compat process:
 *
 *   1. a 16K UFFDIO_COPY reports copy == 16K,
 *   2. all four destination 4K pages read back the bytes that were sent,
 *   3. RssAnon grows by exactly 16 kB across that ioctl -- 64 kB is the
 *      singleton (pre-K1) signature, since RssAnon counts native folios,
 *   4. /proc/self/pagemap reports the same pfn for the four consecutive 4K
 *      pages, i.e. they really share one native folio (needs CAP_SYS_ADMIN;
 *      skipped when pfns read back as zero),
 *   5. the fallback keeps the old stop-at-first-EEXIST semantics: a 16K COPY
 *      across a tuple whose second slice is already populated must report
 *      copy == 4096 and must not disturb the populated slice,
 *   6. a source missing fault exercises the mmap-lock-outside retry and still
 *      installs one correct packed tuple,
 *   7. two concurrent COPYs to the same empty tuple produce exactly one 16K
 *      winner and one -EEXIST loser, without mixing their source bytes.
 *
 * Run it on a pre-K1 kernel and leg 3 fails with 65536 (and leg 4 with four
 * distinct pfns); legs 5 and 6 pass on both, which is the point of them.
 */
#define _GNU_SOURCE

#include <pthread.h>

#include "p3s_tuple_test.h"
#include "p3s_harness.h"

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

#define RACE_ITERATIONS	128UL
#define FIXED_TUPLES	5UL
#define REGION_LEN	((FIXED_TUPLES + RACE_ITERATIONS) * NATIVE_PAGE_SIZE)

struct copy_args {
	pthread_barrier_t *barrier;
	void *dst;
	void *src;
	unsigned long len;
	long copied;
	int uffd;
};

static bool read_page_sizes(uintptr_t address, unsigned long *kernel_size,
			    unsigned long *mmu_size)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	*kernel_size = 0;
	*mmu_size = 0;
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1)
			*kernel_size = size_kb * 1024;
		else if (sscanf(line, "MMUPageSize: %lu kB", &size_kb) == 1)
			*mmu_size = size_kb * 1024;
		if (*kernel_size && *mmu_size)
			break;
	}
	free(line);
	fclose(file);
	return *kernel_size && *mmu_size;
}

static bool has_p3s_geometry(void)
{
	unsigned long kernel_size, mmu_size;
	unsigned char *probe;
	bool parsed;

	probe = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (probe == MAP_FAILED)
		return false;
	*probe = 0x5a;
	parsed = read_page_sizes((uintptr_t)probe, &kernel_size, &mmu_size);
	munmap(probe, NATIVE_PAGE_SIZE);
	if (!parsed)
		return false;

	return kernel_size == NATIVE_PAGE_SIZE && mmu_size == PROCESS_PAGE_SIZE;
}

static void *copy_thread(void *data)
{
	struct copy_args *args = data;

	if (args->barrier)
		pthread_barrier_wait(args->barrier);
	args->copied = uffd_copy(args->uffd, args->dst, args->src, args->len);
	return NULL;
}

static bool test_source_retry(int dst_uffd, unsigned char *dst)
{
	unsigned char resident;
	unsigned char *source;
	long copied;
	bool cold;

	/* The first source PTE is absent, forcing the mmap-lock-outside retry. */
	source = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		return false;
	resident = 1;
	cold = !mincore(source, PROCESS_PAGE_SIZE, &resident) && !(resident & 1);
	copied = cold ? uffd_copy(dst_uffd, dst, source, NATIVE_PAGE_SIZE) : -1;
	cold = cold && copied == NATIVE_PAGE_SIZE &&
	       !memcmp(dst, source, NATIVE_PAGE_SIZE);
	munmap(source, NATIVE_PAGE_SIZE);
	return cold;
}

static bool race_one_tuple(int uffd, unsigned char *dst,
			   unsigned char *source_a, unsigned char *source_b,
			   unsigned long iteration)
{
	pthread_barrier_t barrier;
	struct copy_args args[2] = {
		{
			.dst = dst,
			.src = source_a,
			.len = NATIVE_PAGE_SIZE,
			.copied = -1,
			.uffd = uffd,
		},
		{
			.dst = dst,
			.src = source_b,
			.len = NATIVE_PAGE_SIZE,
			.copied = -1,
			.uffd = uffd,
		},
	};
	pthread_t threads[2];
	unsigned char *winner;
	bool a_won, b_won;

	if (pthread_barrier_init(&barrier, NULL, 3))
		return false;
	args[0].barrier = &barrier;
	args[1].barrier = &barrier;
	if (pthread_create(&threads[0], NULL, copy_thread, &args[0]) ||
	    pthread_create(&threads[1], NULL, copy_thread, &args[1]))
		ksft_exit_fail_msg("race thread creation failed\n");
	pthread_barrier_wait(&barrier);
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	pthread_barrier_destroy(&barrier);

	a_won = args[0].copied == NATIVE_PAGE_SIZE && args[1].copied == -EEXIST;
	b_won = args[1].copied == NATIVE_PAGE_SIZE && args[0].copied == -EEXIST;
	winner = a_won ? source_a : source_b;
	if ((!a_won && !b_won) || memcmp(dst, winner, NATIVE_PAGE_SIZE)) {
		ksft_print_msg("race %lu: copy results %ld/%ld\n", iteration,
			       args[0].copied, args[1].copied);
		return false;
	}
	return true;
}

static bool test_same_tuple_race(int uffd, unsigned char *race_region,
				 unsigned char *source_a,
				 unsigned char *source_b)
{
	for (unsigned long i = 0; i < RACE_ITERATIONS; i++) {
		if (!race_one_tuple(uffd, race_region + i * NATIVE_PAGE_SIZE,
				    source_a, source_b, i))
			return false;
	}
	return true;
}

/*
 * ART's userfaultfd GC mixes UFFDIO_ZEROPAGE and 4K UFFDIO_COPY slices in one
 * tuple, then mremap()s the live part of the space -- a 4K, not 16K, multiple
 * -- with MREMAP_DONTUNMAP.  @tuples spans two tuples: the first is moved
 * whole and the second only partly, so the whole one is regrouped purely
 * because of its zero-page slice.  Every moved slice must keep its bytes.
 */
static bool test_zeropage_mremap(int uffd, unsigned char *tuples,
				 const unsigned char *src)
{
	struct uffdio_zeropage zp = {
		.range = {
			.start = (uintptr_t)(tuples + 2 * PROCESS_PAGE_SIZE),
			.len = PROCESS_PAGE_SIZE,
		},
	};
	const size_t moved_len = (P3S_SLICES + 1) * PROCESS_PAGE_SIZE;
	unsigned char *reservation, *dst;
	unsigned int i;
	bool ok;

	if (ioctl(uffd, UFFDIO_ZEROPAGE, &zp) || zp.zeropage != (long)PROCESS_PAGE_SIZE)
		return false;
	for (i = 0; i <= P3S_SLICES; i++) {
		if (i == 2)
			continue;
		if (uffd_copy(uffd, tuples + i * PROCESS_PAGE_SIZE,
			      (void *)(src + i * PROCESS_PAGE_SIZE),
			      PROCESS_PAGE_SIZE) != (long)PROCESS_PAGE_SIZE)
			return false;
	}

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;
	dst = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	if (mremap(tuples, moved_len, moved_len,
		   MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, dst) != dst) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		return false;
	}
	ok = true;
	for (i = 0; i <= P3S_SLICES; i++)
		ok = ok && all_bytes_are(dst + i * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
					 i == 2 ? 0 : src[i * PROCESS_PAGE_SIZE]);
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
	return ok;
}

FIXTURE(copy_tuple) {
	unsigned char *reservation;
	unsigned char *region;
	unsigned char *source;
	int uffd;
};

FIXTURE_SETUP(copy_tuple)
{
	unsigned int i;

	if (!has_p3s_geometry())
		SKIP(return, "need native-16K/compat-4K P3S geometry");
	self->reservation = mmap(NULL, REGION_LEN + NATIVE_PAGE_SIZE,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	self->region = (unsigned char *)(((uintptr_t)self->reservation +
					    NATIVE_PAGE_SIZE - 1) &
					   ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	self->source = mmap(NULL, 4 * NATIVE_PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->source, MAP_FAILED);
	for (i = 0; i < 2 * P3S_SLICES; i++)
		memset(self->source + i * PROCESS_PAGE_SIZE,
		       0xa0 + i, PROCESS_PAGE_SIZE);
	memset(self->source + 2 * NATIVE_PAGE_SIZE, 0x5a, NATIVE_PAGE_SIZE);
	memset(self->source + 3 * NATIVE_PAGE_SIZE, 0xa5, NATIVE_PAGE_SIZE);
	self->uffd = uffd_open_register(self->region, REGION_LEN);
	if (self->uffd < 0)
		SKIP(return, "userfaultfd MISSING registration failed: %s",
		     strerror(errno));
}

FIXTURE_TEARDOWN(copy_tuple)
{
	close(self->uffd);
	munmap(self->source, 4 * NATIVE_PAGE_SIZE);
	munmap(self->reservation, REGION_LEN + NATIVE_PAGE_SIZE);
}

TEST_F(copy_tuple, installs_whole_native_tuple)
{
	long copied = uffd_copy(self->uffd, self->region, self->source,
				NATIVE_PAGE_SIZE);

	EXPECT_EQ(NATIVE_PAGE_SIZE, copied);
}

TEST_F(copy_tuple, all_four_destination_slices_keep_source_bytes)
{
	unsigned int i;

	ASSERT_EQ(NATIVE_PAGE_SIZE,
		  uffd_copy(self->uffd, self->region, self->source,
			    NATIVE_PAGE_SIZE));
	for (i = 0; i < P3S_SLICES; i++)
		EXPECT_TRUE(all_bytes_are(self->region + i * PROCESS_PAGE_SIZE,
					  PROCESS_PAGE_SIZE, 0xa0 + i));
}

TEST_F(copy_tuple, rss_grows_by_one_native_folio)
{
	long before, after;

	ASSERT_GE(rss_anon_bytes(), 0);
	before = rss_anon_bytes();
	ASSERT_EQ(NATIVE_PAGE_SIZE,
		  uffd_copy(self->uffd, self->region, self->source,
			    NATIVE_PAGE_SIZE));
	after = rss_anon_bytes();
	if (before < 0 || after < 0)
		SKIP(return, "RssAnon unreadable");
	EXPECT_EQ(NATIVE_PAGE_SIZE, after - before);
}

TEST_F(copy_tuple, destination_slices_share_one_native_pfn)
{
	uint64_t pfn[P3S_SLICES];
	unsigned int i;

	ASSERT_EQ(NATIVE_PAGE_SIZE,
		  uffd_copy(self->uffd, self->region, self->source,
			    NATIVE_PAGE_SIZE));
	if (!read_pfns(self->region, pfn))
		SKIP(return, "destination pagemap entries not all present");
	if (!pfn[0])
		SKIP(return, "PFNs hidden (need CAP_SYS_ADMIN)");
	for (i = 1; i < P3S_SLICES; i++)
		EXPECT_EQ(pfn[0], pfn[i]);
}

TEST_F(copy_tuple, populated_sibling_stops_copy_at_first_eexist)
{
	unsigned char *tuple = self->region + NATIVE_PAGE_SIZE;
	long copied;

	ASSERT_EQ(PROCESS_PAGE_SIZE,
		  uffd_copy(self->uffd, tuple + PROCESS_PAGE_SIZE,
			    self->source + NATIVE_PAGE_SIZE,
			    PROCESS_PAGE_SIZE));
	copied = uffd_copy(self->uffd, tuple, self->source, NATIVE_PAGE_SIZE);
	EXPECT_EQ(PROCESS_PAGE_SIZE, copied);
	EXPECT_TRUE(all_bytes_are(tuple, PROCESS_PAGE_SIZE, 0xa0));
	EXPECT_TRUE(all_bytes_are(tuple + PROCESS_PAGE_SIZE,
				      PROCESS_PAGE_SIZE, 0xa4));
}

TEST_F(copy_tuple, missing_source_fault_retries_outside_mmap_lock)
{
	EXPECT_TRUE(test_source_retry(self->uffd,
				      self->region + 2 * NATIVE_PAGE_SIZE));
}

TEST_F(copy_tuple, concurrent_copy_has_one_winner)
{
	EXPECT_TRUE(test_same_tuple_race(self->uffd,
			self->region + FIXED_TUPLES * NATIVE_PAGE_SIZE,
			self->source + 2 * NATIVE_PAGE_SIZE,
			self->source + 3 * NATIVE_PAGE_SIZE));
}

TEST_F(copy_tuple, unaligned_mremap_keeps_copy_slices_beside_zero)
{
	EXPECT_TRUE(test_zeropage_mremap(self->uffd,
			self->region + 3 * NATIVE_PAGE_SIZE, self->source));
}

P3S_HARNESS_COMPAT_MAIN
