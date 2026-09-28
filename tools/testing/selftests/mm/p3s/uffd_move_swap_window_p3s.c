// SPDX-License-Identifier: GPL-2.0
/* Move whole swapped windows and preserve progress at a destination conflict. */
#define _GNU_SOURCE

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define TEST_LENGTH (3 * NATIVE_PAGE_SIZE)

static bool check_bytes(const unsigned char *base, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (base[i] != (unsigned char)(0x31 + i / NATIVE_PAGE_SIZE))
			return false;
	return true;
}

static bool source_swapped(int pagemap, const unsigned char *source,
			   size_t length)
{
	for (size_t off = 0; off < length; off += PROCESS_PAGE_SIZE) {
		uint64_t entry = pagemap_get_entry(pagemap, (char *)source + off);

		if (!(entry & PM_SWAP) || (entry & PM_PRESENT))
			return false;
	}
	return true;
}

FIXTURE(swapped_move) {
	unsigned char *src_reservation;
	unsigned char *dst_reservation;
	unsigned char *src;
	unsigned char *dst;
	int uffd;
	int pagemap;
	bool registered;
};

FIXTURE_VARIANT(swapped_move) {
	bool conflict;
};

FIXTURE_VARIANT_ADD(swapped_move, complete) { .conflict = false };
FIXTURE_VARIANT_ADD(swapped_move, destination_conflict) { .conflict = true };

FIXTURE_SETUP(swapped_move)
{
	struct uffdio_api api = { .api = UFFD_API };

	self->uffd = -1;
	self->pagemap = -1;
	self->src_reservation = MAP_FAILED;
	self->dst_reservation = MAP_FAILED;
	if (!p3s_swap_available())
		SKIP(return, "active swap is required");
	self->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (self->uffd < 0)
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	if (!(api.features & UFFD_FEATURE_MOVE))
		SKIP(return, "UFFDIO_MOVE unavailable");
	self->pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->pagemap, 0);
	self->src = map_exact_edge(TEST_LENGTH, &self->src_reservation);
	self->dst = map_exact_edge(TEST_LENGTH, &self->dst_reservation);
	ASSERT_NE(self->src, MAP_FAILED);
	ASSERT_NE(self->dst, MAP_FAILED);
	for (size_t i = 0; i < TEST_LENGTH; i++)
		self->src[i] = 0x31 + i / NATIVE_PAGE_SIZE;
	if (variant->conflict)
		memset(self->dst + 2 * NATIVE_PAGE_SIZE, 0xa5,
		       NATIVE_PAGE_SIZE);
	ASSERT_EQ(0, uffd_register(self->uffd, self->dst, TEST_LENGTH,
				   true, false, false));
	self->registered = true;
	if (!pageout_reaches_swap(self->src + NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE))
		SKIP(return, "MADV_PAGEOUT did not swap out window");
}

FIXTURE_TEARDOWN(swapped_move)
{
	if (self->registered)
		EXPECT_EQ(0, uffd_unregister(self->uffd, self->dst,
					     TEST_LENGTH));
	if (self->src_reservation != MAP_FAILED)
		munmap(self->src_reservation, TEST_LENGTH + 2 * NATIVE_PAGE_SIZE);
	if (self->dst_reservation != MAP_FAILED)
		munmap(self->dst_reservation, TEST_LENGTH + 2 * NATIVE_PAGE_SIZE);
	if (self->pagemap >= 0)
		close(self->pagemap);
	if (self->uffd >= 0)
		close(self->uffd);
}

static int move_three_windows(const struct _test_data_swapped_move *self,
			      struct uffdio_move *move)
{
	*move = (struct uffdio_move) {
		.src = (uintptr_t)self->src,
		.dst = (uintptr_t)self->dst,
		.len = TEST_LENGTH,
	};
	errno = 0;
	return ioctl(self->uffd, UFFDIO_MOVE, move);
}

static bool move_three_windows_matches(bool conflict, int ret, int error,
				       const struct uffdio_move *move)
{
	return conflict ? ret == -1 && error == EAGAIN &&
			  move->move == 2 * NATIVE_PAGE_SIZE :
			  !ret && move->move == TEST_LENGTH;
}

TEST_F(swapped_move, middle_source_ptes_are_swapped)
{
	EXPECT_TRUE(source_swapped(self->pagemap, self->src + NATIVE_PAGE_SIZE,
				   NATIVE_PAGE_SIZE));
}

TEST_F(swapped_move, reports_progress_through_swapped_window)
{
	struct uffdio_move move;
	int ret = move_three_windows(self, &move);
	int error = errno;

	EXPECT_TRUE(move_three_windows_matches(variant->conflict, ret, error,
					       &move));
}

TEST_F(swapped_move, destination_keeps_source_byte_order)
{
	struct uffdio_move move;
	int ret = move_three_windows(self, &move);
	int error = errno;

	ASSERT_TRUE(move_three_windows_matches(variant->conflict, ret, error,
						&move));
	EXPECT_TRUE(check_bytes(self->dst, variant->conflict ?
				2 * NATIVE_PAGE_SIZE : TEST_LENGTH));
}

TEST_F(swapped_move, unmoved_suffix_or_empty_source_remains_intact)
{
	struct uffdio_move move;
	bool ok = true;
	int ret = move_three_windows(self, &move);
	int error = errno;

	ASSERT_TRUE(move_three_windows_matches(variant->conflict, ret, error,
						&move));
	if (variant->conflict) {
		for (size_t i = 0; i < NATIVE_PAGE_SIZE; i++)
			ok &= self->src[2 * NATIVE_PAGE_SIZE + i] == 0x33 &&
			      self->dst[2 * NATIVE_PAGE_SIZE + i] == 0xa5;
	} else {
		for (size_t off = 0; off < TEST_LENGTH;
		     off += PROCESS_PAGE_SIZE) {
			uint64_t entry = pagemap_get_entry(self->pagemap,
						(char *)self->src + off);

			ok &= !(entry & (PM_PRESENT | PM_SWAP));
		}
	}
	EXPECT_TRUE(ok);
}

FIXTURE(occupied_move) {
	unsigned char *src_reservation;
	unsigned char *dst_reservation;
	unsigned char *src;
	unsigned char *dst;
	int uffd;
	int pagemap;
	bool registered;
};

FIXTURE_VARIANT(occupied_move) {
	unsigned int busy;
};

FIXTURE_VARIANT_ADD(occupied_move, first_slice) { .busy = 0 };
FIXTURE_VARIANT_ADD(occupied_move, second_slice) { .busy = 1 };
FIXTURE_VARIANT_ADD(occupied_move, third_slice) { .busy = 2 };
FIXTURE_VARIANT_ADD(occupied_move, fourth_slice) { .busy = 3 };

FIXTURE_SETUP(occupied_move)
{
	struct uffdio_api api = { .api = UFFD_API };

	self->uffd = -1;
	self->pagemap = -1;
	self->src_reservation = MAP_FAILED;
	self->dst_reservation = MAP_FAILED;
	if (!p3s_swap_available())
		SKIP(return, "active swap is required");
	self->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (self->uffd < 0)
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	if (!(api.features & UFFD_FEATURE_MOVE))
		SKIP(return, "UFFDIO_MOVE unavailable");
	self->pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->pagemap, 0);
	self->src = map_exact_edge(NATIVE_PAGE_SIZE, &self->src_reservation);
	self->dst = map_exact_edge(NATIVE_PAGE_SIZE, &self->dst_reservation);
	ASSERT_NE(self->src, MAP_FAILED);
	ASSERT_NE(self->dst, MAP_FAILED);
	memset(self->src, 0x31, NATIVE_PAGE_SIZE);
	memset(self->dst + variant->busy * PROCESS_PAGE_SIZE, 0xa5,
	       PROCESS_PAGE_SIZE);
	ASSERT_EQ(0, uffd_register(self->uffd, self->dst,
				   NATIVE_PAGE_SIZE, true, false, false));
	self->registered = true;
	if (!pageout_reaches_swap(self->src, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE))
		SKIP(return, "MADV_PAGEOUT did not swap out window");
}

FIXTURE_TEARDOWN(occupied_move)
{
	if (self->registered)
		EXPECT_EQ(0, uffd_unregister(self->uffd, self->dst,
					     NATIVE_PAGE_SIZE));
	if (self->src_reservation != MAP_FAILED)
		munmap(self->src_reservation, 3 * NATIVE_PAGE_SIZE);
	if (self->dst_reservation != MAP_FAILED)
		munmap(self->dst_reservation, 3 * NATIVE_PAGE_SIZE);
	if (self->pagemap >= 0)
		close(self->pagemap);
	if (self->uffd >= 0)
		close(self->uffd);
}

static bool occupied_ptes_intact(const struct _test_data_occupied_move *self,
				 unsigned int busy)
{
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off += PROCESS_PAGE_SIZE) {
		uint64_t entry = pagemap_get_entry(self->pagemap,
						(char *)self->src + off);

		if (!(entry & PM_SWAP) || (entry & PM_PRESENT))
			return false;
		entry = pagemap_get_entry(self->pagemap,
					  (char *)self->dst + off);
		if (off == busy * PROCESS_PAGE_SIZE ? !(entry & PM_PRESENT) :
		    !!(entry & (PM_PRESENT | PM_SWAP)))
			return false;
	}
	return true;
}

static int move_occupied(const struct _test_data_occupied_move *self,
			 struct uffdio_move *move)
{
	*move = (struct uffdio_move) {
		.src = (uintptr_t)self->src,
		.dst = (uintptr_t)self->dst,
		.len = NATIVE_PAGE_SIZE,
	};
	errno = 0;
	return ioctl(self->uffd, UFFDIO_MOVE, move);
}

static bool occupied_move_matches(unsigned int busy, int ret, int error,
				  const struct uffdio_move *move)
{
	int expected = busy ? EBUSY : EEXIST;

	return ret == -1 && error == expected && move->move == -expected;
}

TEST_F(occupied_move, swapped_source_and_one_occupied_destination)
{
	EXPECT_TRUE(occupied_ptes_intact(self, variant->busy));
}

TEST_F(occupied_move, distinguishes_busy_sibling_from_target)
{
	struct uffdio_move move;
	int ret = move_occupied(self, &move);
	int error = errno;

	EXPECT_TRUE(occupied_move_matches(variant->busy, ret, error, &move));
}

TEST_F(occupied_move, rejected_move_preserves_source_and_destination_ptes)
{
	struct uffdio_move move;
	int ret = move_occupied(self, &move);
	int error = errno;

	ASSERT_TRUE(occupied_move_matches(variant->busy, ret, error, &move));
	EXPECT_TRUE(occupied_ptes_intact(self, variant->busy));
}

static bool copy_missing_slices(const struct _test_data_occupied_move *self,
				unsigned int busy)
{
	bool copied = true;

	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off += PROCESS_PAGE_SIZE) {
		struct uffdio_copy copy = {
			.src = (uintptr_t)(self->src + off),
			.dst = (uintptr_t)(self->dst + off),
			.len = PROCESS_PAGE_SIZE,
		};
		int ret;

		errno = 0;
		ret = ioctl(self->uffd, UFFDIO_COPY, &copy);
		copied &= off == busy * PROCESS_PAGE_SIZE ?
			ret == -1 && errno == EEXIST && copy.copy == -EEXIST :
			!ret && copy.copy == (__s64)PROCESS_PAGE_SIZE;
	}
	return copied;
}

TEST_F(occupied_move, explicit_copy_fills_only_missing_slices)
{
	EXPECT_TRUE(copy_missing_slices(self, variant->busy));
}

TEST_F(occupied_move, copy_preserves_source_and_occupied_slice_bytes)
{
	bool ok = true;

	ASSERT_TRUE(copy_missing_slices(self, variant->busy));
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off++)
		ok &= self->src[off] == 0x31 && self->dst[off] ==
			(off / PROCESS_PAGE_SIZE == variant->busy ? 0xa5 : 0x31);
	EXPECT_TRUE(ok);
}

P3S_HARNESS_4KB_MAIN
