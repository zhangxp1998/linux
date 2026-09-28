// SPDX-License-Identifier: GPL-2.0
/* MADV_FREE must cover mapped slices across holes, not just one PTE run. */
#define _GNU_SOURCE

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_slice_test.h"
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define RECLAIM_ROUNDS 8

struct window {
	unsigned char *area;
	unsigned char *base;
	unsigned int mask;
};

static void *scratch;

static void drain(void)
{
	if (madvise(scratch, NATIVE_PAGE_SIZE, MADV_COLD))
		ksft_exit_fail_msg("scratch MADV_COLD: %s\n", strerror(errno));
}

static unsigned int first_slice(unsigned int mask)
{
	return __builtin_ctz(mask);
}

static unsigned int last_slice(unsigned int mask)
{
	return 31 - __builtin_clz(mask);
}

static bool data_matches(struct window *w, bool zero, int redirty)
{
	unsigned int i;
	size_t j;

	for (i = 0; i < P3S_SLICES; i++) {
		unsigned char expected = zero ? 0 : (int)i == redirty ? 0xa5 : 0x40 + i;

		if (!(w->mask & (1U << i)))
			continue;
		for (j = 0; j < PROCESS_PAGE_SIZE; j++)
			if (((volatile unsigned char *)w->base)[i * PROCESS_PAGE_SIZE + j] != expected)
				return false;
	}
	return true;
}

static struct window setup(unsigned int mask, bool zap)
{
	struct window w = { .mask = mask };
	uintptr_t address;
	uint64_t first = 0, pfn = 0, entry;
	unsigned int i;
	bool valid = true;

	w.area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (w.area == MAP_FAILED)
		ksft_exit_fail_msg("reserve: %s\n", strerror(errno));
	address = ((uintptr_t)w.area + 2 * NATIVE_PAGE_SIZE - 1) &
		  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	w.base = mmap((void *)address, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (w.base == MAP_FAILED)
		ksft_exit_fail_msg("map: %s\n", strerror(errno));
	for (i = 0; i < P3S_SLICES; i++) {
		if (zap || (mask & (1U << i)))
			memset(w.base + i * PROCESS_PAGE_SIZE, 0x40 + i, PROCESS_PAGE_SIZE);
	}
	if (zap) {
		for (i = 0; i < P3S_SLICES; i++)
			if (!(mask & (1U << i)) &&
			    madvise(w.base + i * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE, MADV_DONTNEED))
				ksft_exit_fail_msg("make hole: %s\n", strerror(errno));
	}
	for (i = 0; i < P3S_SLICES; i++) {
		if (!p3s_pagemap_entry(w.base + i * PROCESS_PAGE_SIZE, &entry))
			ksft_exit_fail_msg("pagemap read failed\n");
		if (!(mask & (1U << i))) {
			valid &= !(entry & (PM_PRESENT | PM_SWAP));
			continue;
		}
		valid &= p3s_pfn(w.base + i * PROCESS_PAGE_SIZE, &pfn);
		if (!first)
			first = pfn;
		valid &= pfn == first;
	}
	if (!valid || !first)
		ksft_exit_fail_msg("fixture is not the requested sliced folio\n");
	return w;
}

static bool lazy_bytes(struct window *w, unsigned long expected)
{
	unsigned long value;

	drain();
	return p3s_smaps_bytes(w->base, 1, "LazyFree", &value) && value == expected;
}

static bool discarded(struct window *w)
{
	uint64_t entry;
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		if ((w->mask & (1U << i)) &&
		    (!p3s_pagemap_entry(w->base + i * PROCESS_PAGE_SIZE, &entry) ||
		     (entry & (PM_PRESENT | PM_SWAP))))
			return false;
	return true;
}

static void reclaim(struct window *w)
{
	unsigned int round;

	for (round = 0; round < RECLAIM_ROUNDS; round++) {
		if (madvise(w->base, NATIVE_PAGE_SIZE, MADV_PAGEOUT))
			ksft_exit_fail_msg("MADV_PAGEOUT: %s\n", strerror(errno));
		if (discarded(w))
			break;
		usleep(10000);
	}
}

static void release(struct window *w)
{
	munmap(w->area, 4 * NATIVE_PAGE_SIZE);
}

FIXTURE(lazyfree_holes) {
	struct window w;
	unsigned int first;
	unsigned int end;
};

FIXTURE_VARIANT(lazyfree_holes) {
	unsigned int mask;
	bool zap;
};

FIXTURE_VARIANT_ADD(lazyfree_holes, mask5) { .mask = 5 };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask5_zapped) { .mask = 5, .zap = true };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask9) { .mask = 9 };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask9_zapped) { .mask = 9, .zap = true };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask10) { .mask = 10 };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask10_zapped) { .mask = 10, .zap = true };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask11) { .mask = 11 };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask11_zapped) { .mask = 11, .zap = true };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask13) { .mask = 13 };
FIXTURE_VARIANT_ADD(lazyfree_holes, mask13_zapped) { .mask = 13, .zap = true };

FIXTURE_SETUP(lazyfree_holes)
{
	self->w = setup(variant->mask, variant->zap);
	self->first = variant->zap ? first_slice(variant->mask) : 0;
	self->end = variant->zap ? last_slice(variant->mask) + 1 : P3S_SLICES;
}

FIXTURE_TEARDOWN(lazyfree_holes)
{
	release(&self->w);
}

static int mark_lazyfree(const struct _test_data_lazyfree_holes *self)
{
	return madvise(self->w.base + self->first * PROCESS_PAGE_SIZE,
		       (self->end - self->first) * PROCESS_PAGE_SIZE, MADV_FREE);
}

TEST_F(lazyfree_holes, every_mapped_slice_is_lazyfree)
{
	ASSERT_EQ(0, mark_lazyfree(self));
	EXPECT_TRUE(lazy_bytes(&self->w,
			       __builtin_popcount(variant->mask) * PROCESS_PAGE_SIZE));
}

TEST_F(lazyfree_holes, data_remains_until_reclaim)
{
	ASSERT_EQ(0, mark_lazyfree(self));
	EXPECT_TRUE(data_matches(&self->w, false, -1));
}

TEST_F(lazyfree_holes, pageout_discards_mapped_ptes_without_swap)
{
	ASSERT_EQ(0, mark_lazyfree(self));
	reclaim(&self->w);
	EXPECT_TRUE(discarded(&self->w));
}

TEST_F(lazyfree_holes, refault_zero_fills_discarded_slices)
{
	ASSERT_EQ(0, mark_lazyfree(self));
	reclaim(&self->w);
	ASSERT_TRUE(discarded(&self->w));
	EXPECT_TRUE(data_matches(&self->w, true, -1));
}

FIXTURE(partial_free) {
	struct window w;
};

FIXTURE_VARIANT(partial_free) {
	unsigned int first;
};

FIXTURE_VARIANT_ADD(partial_free, first_slice) { .first = 0 };
FIXTURE_VARIANT_ADD(partial_free, third_slice) { .first = 2 };

FIXTURE_SETUP(partial_free)
{
	self->w = setup(5, false);
}

FIXTURE_TEARDOWN(partial_free)
{
	release(&self->w);
}

TEST_F(partial_free, excludes_mapping_outside_requested_slice)
{
	ASSERT_EQ(0, madvise(self->w.base + variant->first * PROCESS_PAGE_SIZE,
			     PROCESS_PAGE_SIZE, MADV_FREE));
	EXPECT_TRUE(lazy_bytes(&self->w, 0));
}

TEST_F(partial_free, reclaim_preserves_both_slices)
{
	ASSERT_EQ(0, madvise(self->w.base + variant->first * PROCESS_PAGE_SIZE,
			     PROCESS_PAGE_SIZE, MADV_FREE));
	reclaim(&self->w);
	EXPECT_TRUE(data_matches(&self->w, false, -1));
}

FIXTURE(shared_free) {
	struct window w;
};

FIXTURE_SETUP(shared_free)
{
	self->w = setup(5, false);
}

FIXTURE_TEARDOWN(shared_free)
{
	release(&self->w);
}

struct shared_result {
	bool policy;
	bool parent_data;
	bool child_data;
};

static struct shared_result run_shared(struct window *w)
{
	struct shared_result result = {};
	int ready[2], done[2], status;
	char byte = 1;
	pid_t child;
	int ret;

	if (pipe(ready) || pipe(done))
		ksft_exit_fail_msg("pipe: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (write(ready[1], &byte, 1) != 1 ||
		    read(done[0], &byte, 1) != 1)
			_exit(2);
		_exit(data_matches(w, false, -1) ? 0 : 1);
	}
	close(ready[1]);
	close(done[0]);
	if (read(ready[0], &byte, 1) != 1)
		ksft_exit_fail_msg("child readiness failed\n");
	ret = madvise(w->base, NATIVE_PAGE_SIZE, MADV_FREE);
	result.policy = !ret && lazy_bytes(w, 0);
	reclaim(w);
	result.parent_data = data_matches(w, false, -1);
	if (write(done[1], &byte, 1) != 1 ||
	    waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("child completion failed\n");
	result.child_data = WIFEXITED(status) && !WEXITSTATUS(status);
	close(ready[0]);
	close(done[1]);
	return result;
}

TEST_F(shared_free, shared_slices_are_not_marked_lazyfree)
{
	struct shared_result result = run_shared(&self->w);

	EXPECT_TRUE(result.policy);
}

TEST_F(shared_free, parent_data_survives_reclaim)
{
	struct shared_result result = run_shared(&self->w);

	EXPECT_TRUE(result.parent_data);
}

TEST_F(shared_free, child_data_survives_reclaim)
{
	struct shared_result result = run_shared(&self->w);

	EXPECT_TRUE(result.child_data);
}

FIXTURE(redirty_free) {
	struct window w;
};

FIXTURE_VARIANT(redirty_free) {
	unsigned int dirty;
};

FIXTURE_VARIANT_ADD(redirty_free, first_slice) { .dirty = 0 };
FIXTURE_VARIANT_ADD(redirty_free, third_slice) { .dirty = 2 };

FIXTURE_SETUP(redirty_free)
{
	self->w = setup(5, false);
}

FIXTURE_TEARDOWN(redirty_free)
{
	release(&self->w);
}

TEST_F(redirty_free, whole_folio_starts_lazyfree)
{
	ASSERT_EQ(0, madvise(self->w.base, NATIVE_PAGE_SIZE, MADV_FREE));
	EXPECT_TRUE(lazy_bytes(&self->w,
			       __builtin_popcount(self->w.mask) * PROCESS_PAGE_SIZE));
}

TEST_F(redirty_free, written_slice_survives_reclaim)
{
	bool preserved = true;

	ASSERT_EQ(0, madvise(self->w.base, NATIVE_PAGE_SIZE, MADV_FREE));
	ASSERT_TRUE(lazy_bytes(&self->w,
			       __builtin_popcount(self->w.mask) * PROCESS_PAGE_SIZE));
	memset(self->w.base + variant->dirty * PROCESS_PAGE_SIZE, 0xa5, PROCESS_PAGE_SIZE);
	reclaim(&self->w);
	for (unsigned int i = 0; i < P3S_SLICES; i++) {
		volatile unsigned char *data = self->w.base + i * PROCESS_PAGE_SIZE;
		unsigned char expected;

		if (!(self->w.mask & (1U << i)))
			continue;
		expected = i == variant->dirty ? 0xa5 : data[0];
		if (i != variant->dirty && expected != 0 &&
		    expected != 0x40 + i)
			preserved = false;
		for (size_t j = 0; j < PROCESS_PAGE_SIZE; j++)
			if (data[j] != expected)
				preserved = false;
	}
	EXPECT_TRUE(preserved);
}

FIXTURE(packed_lazyfree) {
	unsigned char *map, *reservation;
};

FIXTURE_SETUP(packed_lazyfree)
{
	self->reservation = MAP_FAILED;
	self->map = map_aligned(NATIVE_PAGE_SIZE, &self->reservation);
	ASSERT_NE(MAP_FAILED, self->map);
	fill_slices(self->map, 0x31);
}

FIXTURE_TEARDOWN(packed_lazyfree)
{
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, 2 * NATIVE_PAGE_SIZE);
}

TEST_F(packed_lazyfree, redirtied_page_reaches_swap_and_refaults)
{
	uint64_t before[P3S_SLICES], after[P3S_SLICES];
	unsigned long swap = 0;
	unsigned int attempt;

	if (!p3s_swap_available())
		SKIP(return, "no active swap");
	if (!read_pfns(self->map, before) || !before[0])
		SKIP(return, "pagemap PFNs unavailable");
	ASSERT_TRUE(same_pfn(before));
	ASSERT_EQ(0, madvise(self->map, NATIVE_PAGE_SIZE, MADV_FREE));
	ASSERT_TRUE(read_pfns(self->map, after));
	EXPECT_EQ(0, memcmp(before, after, sizeof(before)));
	fill_slices(self->map, 0x61);
	/* Redirtying can defer the first PAGEOUT; retry with a bounded 3s wait. */
	for (attempt = 0; attempt < 300; attempt++) {
		if (!(attempt % 10))
			ASSERT_EQ(0, madvise(self->map, NATIVE_PAGE_SIZE, MADV_PAGEOUT));
		ASSERT_TRUE(p3s_smaps_bytes(self->map, NATIVE_PAGE_SIZE, "Swap", &swap));
		if (swap == NATIVE_PAGE_SIZE)
			break;
		usleep(10000);
	}
	if (swap != NATIVE_PAGE_SIZE) {
		ASSERT_TRUE(check_slices(self->map, 0x61));
		SKIP(return, "MADV_PAGEOUT did not swap out redirtied page");
	}
	EXPECT_TRUE(check_slices(self->map, 0x61));
	ASSERT_TRUE(read_pfns(self->map, after));
	EXPECT_TRUE(same_pfn(after));
	ASSERT_TRUE(p3s_smaps_bytes(self->map, NATIVE_PAGE_SIZE, "Swap", &swap));
	EXPECT_EQ(0, swap);
}

static void setup_harness(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	cpu_set_t allowed, one;
	int cpu;

	if (geteuid()) {
		ksft_print_header();
		ksft_exit_skip("root is required to verify folio identity\n");
	}
	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		ksft_exit_fail_msg("get affinity failed\n");
	CPU_ZERO(&one);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed)) {
			CPU_SET(cpu, &one);
			break;
		}
	if (sched_setaffinity(0, sizeof(one), &one))
		ksft_exit_fail_msg("set affinity failed\n");
	scratch = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (scratch == MAP_FAILED)
		ksft_exit_fail_msg("scratch mmap failed\n");
	memset(scratch, 1, NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
