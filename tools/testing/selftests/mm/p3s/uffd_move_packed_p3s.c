// SPDX-License-Identifier: GPL-2.0
/* UFFDIO_MOVE preserves whole packed pages and rejects partial-page moves. */
#define _GNU_SOURCE
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define PAGE_COUNT 256
#define LENGTH (PAGE_COUNT * NATIVE_PAGE_SIZE)
#define RACE_COUNT 128

FIXTURE(uffd_move) {
	int uffd;
	unsigned char *src, *dst, *other;
	unsigned char *src_reservation, *dst_reservation, *other_reservation;
};

FIXTURE_SETUP(uffd_move)
{
	struct uffdio_api api = { .api = UFFD_API };
	unsigned char *probe;
	uint64_t entry = 0;

	self->uffd = -1;
	self->src_reservation = self->dst_reservation = MAP_FAILED;
	self->other_reservation = MAP_FAILED;
	self->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (self->uffd < 0 &&
	    (errno == ENOSYS || errno == EPERM || errno == EACCES))
		SKIP(return, "userfaultfd unavailable");
	ASSERT_GE(self->uffd, 0);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	if (!(api.features & UFFD_FEATURE_MOVE))
		SKIP(return, "UFFDIO_MOVE unavailable");
	probe = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, probe);
	probe[0] = 1;
	if (!p3s_pagemap_entry(probe, &entry) || !PAGEMAP_PFN(entry)) {
		munmap(probe, NATIVE_PAGE_SIZE);
		SKIP(return, "pagemap PFNs require CAP_SYS_ADMIN");
	}
	munmap(probe, NATIVE_PAGE_SIZE);
	self->src = map_exact_edge(LENGTH, &self->src_reservation);
	ASSERT_NE(MAP_FAILED, self->src);
	self->dst = map_exact_edge(LENGTH, &self->dst_reservation);
	ASSERT_NE(MAP_FAILED, self->dst);
}

FIXTURE_TEARDOWN(uffd_move)
{
	if (self->uffd >= 0)
		close(self->uffd);
	if (self->src_reservation != MAP_FAILED)
		munmap(self->src_reservation, LENGTH + 2 * NATIVE_PAGE_SIZE);
	if (self->dst_reservation != MAP_FAILED)
		munmap(self->dst_reservation, LENGTH + 2 * NATIVE_PAGE_SIZE);
	if (self->other_reservation != MAP_FAILED)
		munmap(self->other_reservation, LENGTH + 2 * NATIVE_PAGE_SIZE);
}

static void register_missing(struct __test_metadata *_metadata, int uffd,
			     unsigned char *map, size_t len)
{
	ASSERT_EQ(0, uffd_register(uffd, map, len, true, false, false));
}

static void populate_page(struct __test_metadata *_metadata, unsigned char *map,
			  unsigned char first, uint64_t pfn[P3S_SLICES])
{
	fill_slices(map, first);
	ASSERT_TRUE(read_pfns(map, pfn));
	ASSERT_TRUE(same_pfn(pfn));
}

TEST_F(uffd_move, registration_preserves_packed_page)
{
	uint64_t before[P3S_SLICES], after[P3S_SLICES];

	populate_page(_metadata, self->src, 0x31, before);
	register_missing(_metadata, self->uffd, self->src, LENGTH);
	ASSERT_TRUE(read_pfns(self->src, after));
	EXPECT_EQ(0, memcmp(before, after, sizeof(before)));
	EXPECT_TRUE(check_slices(self->src, 0x31));
	ASSERT_EQ(0, uffd_unregister(self->uffd, self->src, LENGTH));
	EXPECT_TRUE(check_slices(self->src, 0x31));
}

static void check_partial_move(struct __test_metadata *_metadata,
			       FIXTURE_DATA(uffd_move) *self,
			       bool register_source, size_t dst_offset)
{
	uint64_t before[P3S_SLICES], after[P3S_SLICES];
	struct uffdio_move move = {
		.src = (uintptr_t)self->src,
		.dst = (uintptr_t)(self->dst + dst_offset),
		.len = PROCESS_PAGE_SIZE,
	};

	populate_page(_metadata, self->src, 0x31, before);
	if (register_source)
		register_missing(_metadata, self->uffd, self->src, LENGTH);
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	errno = 0;
	ASSERT_EQ(-1, ioctl(self->uffd, UFFDIO_MOVE, &move));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_EQ(-EBUSY, move.move);
	EXPECT_FALSE(p3s_page_present(self->dst + dst_offset));
	ASSERT_TRUE(read_pfns(self->src, after));
	EXPECT_EQ(0, memcmp(before, after, sizeof(before)));
	EXPECT_TRUE(check_slices(self->src, 0x31));
}

TEST_F(uffd_move, one_slice_of_shared_page_is_busy)
{
	check_partial_move(_metadata, self, true, 0);
}

TEST_F(uffd_move, cross_slice_partial_move_is_busy)
{
	check_partial_move(_metadata, self, true, PROCESS_PAGE_SIZE);
}

TEST_F(uffd_move, unregistered_source_partial_move_is_busy)
{
	check_partial_move(_metadata, self, false, 0);
}

TEST_F(uffd_move, whole_pages_keep_pfns)
{
	uint64_t before[PAGE_COUNT][P3S_SLICES], after[P3S_SLICES];
	struct uffdio_move move = {
		.src = (uintptr_t)self->src, .dst = (uintptr_t)self->dst,
		.len = LENGTH,
	};
	unsigned int i;

	for (i = 0; i < PAGE_COUNT; i++)
		populate_page(_metadata, self->src + i * NATIVE_PAGE_SIZE, 0x31, before[i]);
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_MOVE, &move));
	ASSERT_EQ(LENGTH, move.move);
	for (i = 0; i < PAGE_COUNT; i++) {
		EXPECT_TRUE(check_slices(self->dst + i * NATIVE_PAGE_SIZE, 0x31));
		ASSERT_TRUE(read_pfns(self->dst + i * NATIVE_PAGE_SIZE, after));
		EXPECT_EQ(0, memcmp(before[i], after, sizeof(after))) TH_LOG("page=%u", i);
	}
}

TEST_F(uffd_move, whole_page_move_preserves_accounting)
{
	uint64_t pfn[P3S_SLICES];
	unsigned long before, source, destination;
	struct uffdio_move move = {
		.src = (uintptr_t)self->src, .dst = (uintptr_t)self->dst,
		.len = LENGTH,
	};
	unsigned int i;

	for (i = 0; i < PAGE_COUNT; i++)
		populate_page(_metadata, self->src + i * NATIVE_PAGE_SIZE, 0x31, pfn);
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	ASSERT_TRUE(p3s_smaps_bytes(self->src, LENGTH, "Anonymous", &before));
	ASSERT_EQ(LENGTH, before);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_MOVE, &move));
	ASSERT_EQ(LENGTH, move.move);
	ASSERT_TRUE(p3s_smaps_bytes(self->src, LENGTH, "Anonymous", &source));
	ASSERT_TRUE(p3s_smaps_bytes(self->dst, LENGTH, "Anonymous", &destination));
	EXPECT_EQ(0, source);
	EXPECT_EQ(before, destination);
}

TEST_F(uffd_move, occupied_destination_slice_rejects_whole_page)
{
	unsigned char conflict[PROCESS_PAGE_SIZE];
	uint64_t pfn[P3S_SLICES];
	struct uffdio_copy copy = {
		.src = (uintptr_t)conflict,
		.dst = (uintptr_t)(self->dst + PROCESS_PAGE_SIZE),
		.len = sizeof(conflict),
	};
	struct uffdio_move move = {
		.src = (uintptr_t)self->src, .dst = (uintptr_t)self->dst,
		.len = NATIVE_PAGE_SIZE,
	};

	populate_page(_metadata, self->src, 0x31, pfn);
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	memset(conflict, 0x91, sizeof(conflict));
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_COPY, &copy));
	ASSERT_EQ(sizeof(conflict), copy.copy);
	ASSERT_EQ(-1, ioctl(self->uffd, UFFDIO_MOVE, &move));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_EQ(-EBUSY, move.move);
	EXPECT_EQ(0x91, self->dst[PROCESS_PAGE_SIZE]);
	EXPECT_FALSE(p3s_page_present(self->dst));
	EXPECT_TRUE(check_slices(self->src, 0x31));
}

TEST_F(uffd_move, source_with_hole_is_busy)
{
	uint64_t pfn[P3S_SLICES];
	struct uffdio_move move = {
		.src = (uintptr_t)self->src, .dst = (uintptr_t)self->dst,
		.len = NATIVE_PAGE_SIZE,
	};

	populate_page(_metadata, self->src, 0x31, pfn);
	ASSERT_EQ(0, madvise(self->src + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	ASSERT_EQ(-1, ioctl(self->uffd, UFFDIO_MOVE, &move));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_EQ(-EBUSY, move.move);
	EXPECT_FALSE(p3s_page_present(self->dst));
	EXPECT_EQ(0x31, self->src[0]);
	EXPECT_EQ(0x33, self->src[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x34, self->src[3 * PROCESS_PAGE_SIZE]);
}

struct move_thread {
	struct uffdio_move move;
	atomic_bool *start;
	int uffd, result, error;
};

static void *racing_move(void *data)
{
	struct move_thread *arg = data;

	while (!atomic_load_explicit(arg->start, memory_order_acquire))
		sched_yield();
	arg->result = ioctl(arg->uffd, UFFDIO_MOVE, &arg->move);
	arg->error = errno;
	return NULL;
}

TEST_F(uffd_move, racing_moves_to_one_page_preserve_winner_and_loser)
{
	uint64_t pfn[P3S_SLICES];
	unsigned int i;

	self->other = map_exact_edge(LENGTH, &self->other_reservation);
	ASSERT_NE(MAP_FAILED, self->other);
	register_missing(_metadata, self->uffd, self->dst, LENGTH);
	for (i = 0; i < RACE_COUNT; i++) {
		unsigned char *a = self->src + i * NATIVE_PAGE_SIZE;
		unsigned char *b = self->other + i * NATIVE_PAGE_SIZE;
		unsigned char *dst = self->dst + i * NATIVE_PAGE_SIZE;
		atomic_bool start = false;
		struct move_thread args[2] = {
			{ .move = { .src = (uintptr_t)a, .dst = (uintptr_t)dst,
				    .len = NATIVE_PAGE_SIZE },
			  .uffd = self->uffd, .start = &start },
			{ .move = { .src = (uintptr_t)b, .dst = (uintptr_t)dst,
				    .len = NATIVE_PAGE_SIZE },
			  .uffd = self->uffd, .start = &start },
		};
		pthread_t threads[2];
		unsigned int winner, loser;
		int ret;

		populate_page(_metadata, a, 0x31, pfn);
		populate_page(_metadata, b, 0x71, pfn);
		ASSERT_EQ(0, pthread_create(&threads[0], NULL, racing_move, &args[0]));
		ret = pthread_create(&threads[1], NULL, racing_move, &args[1]);
		atomic_store_explicit(&start, true, memory_order_release);
		ASSERT_EQ(0, pthread_join(threads[0], NULL));
		if (!ret)
			ASSERT_EQ(0, pthread_join(threads[1], NULL));
		ASSERT_EQ(0, ret);
		winner = args[0].result ? 1 : 0;
		loser = 1 - winner;
		ASSERT_EQ(0, args[winner].result);
		EXPECT_EQ(NATIVE_PAGE_SIZE, args[winner].move.move);
		EXPECT_EQ(-1, args[loser].result);
		EXPECT_EQ(EEXIST, args[loser].error);
		EXPECT_EQ(-EEXIST, args[loser].move.move);
		EXPECT_TRUE(check_slices(dst, winner ? 0x71 : 0x31));
		ASSERT_TRUE(read_pfns(dst, pfn));
		EXPECT_TRUE(same_pfn(pfn));
		EXPECT_TRUE(check_slices(winner ? a : b, winner ? 0x31 : 0x71));
		ASSERT_TRUE(read_pfns(winner ? a : b, pfn));
		EXPECT_TRUE(same_pfn(pfn));
	}
}

P3S_HARNESS_4KB_MAIN
