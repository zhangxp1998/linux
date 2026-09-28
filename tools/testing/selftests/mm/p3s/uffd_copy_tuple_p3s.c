// SPDX-License-Identifier: GPL-2.0
/*
 * UFFDIO_COPY on a 4K compat process walks the request slice by slice and,
 * via p3s_uffd_install_anon_folio() (aosp/4312528), reuses an exclusive
 * native 16K folio already mapped by a sibling slice in the same window.
 *
 * This test verifies from a 4K compat process that:
 *   1. a 16K UFFDIO_COPY installs all four 4K slices into one shared folio,
 *   2. RssAnon increases by one 16K folio and pagemap PFNs match across
 *      the four slices (skipped when pagemap PFNs are hidden),
 *   3. partial copies stop at the first populated sibling (-EEXIST),
 *   4. a cold source retries outside mmap_lock cleanly,
 *   5. concurrent copies on the same tuple produce one winner and one
 *      -EEXIST loser without data corruption, and
 *   6. mixing UFFDIO_ZEROPAGE and 4K UFFDIO_COPY handles partial unaligned
 *      MREMAP_DONTUNMAP moves cleanly.
 */
#define _GNU_SOURCE

#include <pthread.h>

#include "p3s_slice_test.h"
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
	if (pthread_create(&threads[0], NULL, copy_thread, &args[0])) {
		pthread_barrier_destroy(&barrier);
		return false;
	}
	if (pthread_create(&threads[1], NULL, copy_thread, &args[1])) {
		pthread_cancel(threads[0]);
		pthread_join(threads[0], NULL);
		pthread_barrier_destroy(&barrier);
		return false;
	}
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

	self->uffd = -1;
	self->reservation = MAP_FAILED;
	self->source = MAP_FAILED;
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
	if (self->uffd >= 0)
		close(self->uffd);
	if (self->source != MAP_FAILED)
		munmap(self->source, 4 * NATIVE_PAGE_SIZE);
	if (self->reservation != MAP_FAILED)
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

P3S_HARNESS_4KB_MAIN
