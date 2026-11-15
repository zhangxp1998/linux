// SPDX-License-Identifier: GPL-2.0
/* Private /dev/zero keeps file-offset units even though its faults are anonymous. */
#define _GNU_SOURCE
#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "p3s_test.h"
#include "p3s_harness.h"

#define AREA_SIZE (10 * NATIVE_PAGE_SIZE)

static int zero_fd;

static unsigned char *reserve(unsigned char **raw)
{
	*raw = mmap(NULL, AREA_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*raw == MAP_FAILED)
		ksft_exit_fail_msg("reserve: %s\n", strerror(errno));
	return (void *)(((uintptr_t)*raw + 2 * NATIVE_PAGE_SIZE - 1) &
			~(NATIVE_PAGE_SIZE - 1));
}

static void map_zero(unsigned char *addr, size_t len, off_t offset)
{
	if (mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
		 zero_fd, offset) != addr)
		ksft_exit_fail_msg("map /dev/zero: %s\n", strerror(errno));
}

static bool bytes_are(unsigned char *addr, size_t len, unsigned char value)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (addr[i] != value)
			return false;
	return true;
}

static bool grow(unsigned int offset, bool populated, int flags)
{
	size_t unit = getpagesize();
	unsigned char *raw, *base = reserve(&raw), *dst;
	bool ok;

	map_zero(base, 2 * unit, offset * unit);
	if (munmap(base + 2 * unit, unit))
		ksft_exit_fail_msg("unmap growth gap: %s\n", strerror(errno));
	if (populated)
		memset(base, 0x63, 2 * unit);
	dst = mremap(base, 2 * unit, 3 * unit, flags);
	ok = dst == base;
	if (!ok)
		ksft_print_msg("grow offset=%u populated=%d flags=%d: %s\n",
			       offset, populated, flags,
			       dst == MAP_FAILED ? strerror(errno) : "unexpected move");
	if (dst != MAP_FAILED) {
		ok &= bytes_are(dst, 2 * unit, populated ? 0x63 : 0);
		ok &= bytes_are(dst + 2 * unit, unit, 0);
		memset(dst + 2 * unit, 0x97, unit);
		ok &= bytes_are(dst, 2 * unit, populated ? 0x63 : 0);
		ok &= p3s_maps_count(dst, 3 * unit) == 1;
		if (dst != base)
			munmap(dst, 3 * unit);
	}
	munmap(raw, AREA_SIZE);
	return ok;
}

static bool merge_right(bool linear, bool populated)
{
	unsigned char *raw, *base = reserve(&raw);
	size_t len = NATIVE_PAGE_SIZE;
	int expected = linear ? 1 : 2;
	bool ok;

	/* The right neighbour exists first, so mmap tests the merge-next path. */
	map_zero(base + len, len, linear ? len : 4 * len);
	if (populated)
		memset(base + len, 0x83, len);
	map_zero(base, len, 0);
	ok = p3s_maps_count(base, 2 * len) == expected;
	if (!ok)
		ksft_print_msg("right merge linear=%d populated=%d: %d VMAs, expected %d\n",
			       linear, populated, p3s_maps_count(base, 2 * len), expected);
	ok &= bytes_are(base, len, 0);
	ok &= bytes_are(base + len, len, populated ? 0x83 : 0);
	memset(base, 0x51, len);
	memset(base + len, 0x83, len);
	ok &= bytes_are(base, len, 0x51) && bytes_are(base + len, len, 0x83);
	munmap(raw, AREA_SIZE);
	return ok;
}

/* Moving a populated range beside its source must not merge it with itself. */
static bool move_beside_source(unsigned int offset, bool populated)
{
	size_t unit = getpagesize();
	unsigned char *raw, *base = reserve(&raw), *dst;
	bool ok;

	map_zero(base, unit, offset * unit);
	if (populated)
		memset(base, 0x69, unit);
	dst = mremap(base, unit, unit, MREMAP_MAYMOVE | MREMAP_FIXED,
		     base + unit);
	ok = dst == base + unit;
	if (ok) {
		ok &= bytes_are(dst, unit, populated ? 0x69 : 0);
		memset(dst, 0x69, unit);
		ok &= bytes_are(dst, unit, 0x69);
	}
	munmap(raw, AREA_SIZE);
	return ok;
}

FIXTURE(devzero_growth) {
	int fd;
};

FIXTURE_VARIANT(devzero_growth) {
	unsigned int offset;
	bool populated;
};

FIXTURE_VARIANT_ADD(devzero_growth, slice_0_cold) { .offset = 0, .populated = false };
FIXTURE_VARIANT_ADD(devzero_growth, slice_0_hot) { .offset = 0, .populated = true };
FIXTURE_VARIANT_ADD(devzero_growth, slice_1_cold) { .offset = 1, .populated = false };
FIXTURE_VARIANT_ADD(devzero_growth, slice_1_hot) { .offset = 1, .populated = true };
FIXTURE_VARIANT_ADD(devzero_growth, slice_2_cold) { .offset = 2, .populated = false };
FIXTURE_VARIANT_ADD(devzero_growth, slice_2_hot) { .offset = 2, .populated = true };
FIXTURE_VARIANT_ADD(devzero_growth, slice_3_cold) { .offset = 3, .populated = false };
FIXTURE_VARIANT_ADD(devzero_growth, slice_3_hot) { .offset = 3, .populated = true };

FIXTURE_SETUP(devzero_growth)
{
	if (!p3s_is_compat_process() && variant->offset)
		SKIP(return, "native geometry has no 4KB slice offset");
	self->fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	zero_fd = self->fd;
}

FIXTURE_TEARDOWN(devzero_growth)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(devzero_growth, grow_in_place)
{
	EXPECT_TRUE(grow(variant->offset, variant->populated, 0));
}

TEST_F(devzero_growth, grow_with_maymove)
{
	EXPECT_TRUE(grow(variant->offset, variant->populated, MREMAP_MAYMOVE));
}

TEST_F(devzero_growth, move_beside_source)
{
	EXPECT_TRUE(move_beside_source(variant->offset, variant->populated));
}

FIXTURE(devzero_right) {
	int fd;
};

FIXTURE_VARIANT(devzero_right) {
	bool linear;
	bool populated;
};

FIXTURE_VARIANT_ADD(devzero_right, linear_cold) { .linear = true, .populated = false };
FIXTURE_VARIANT_ADD(devzero_right, linear_hot) { .linear = true, .populated = true };
FIXTURE_VARIANT_ADD(devzero_right, nonlinear_cold) { .linear = false, .populated = false };
FIXTURE_VARIANT_ADD(devzero_right, nonlinear_hot) { .linear = false, .populated = true };

FIXTURE_SETUP(devzero_right)
{
	self->fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	zero_fd = self->fd;
}

FIXTURE_TEARDOWN(devzero_right)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(devzero_right, adjacent_file_offsets)
{
	EXPECT_TRUE(merge_right(variant->linear, variant->populated));
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
