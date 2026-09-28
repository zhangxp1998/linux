// SPDX-License-Identifier: GPL-2.0
/*
 * A discarded 4K anonymous page must read as zero even while sibling slices
 * of its native folio remain mapped.  The first touch is normally a write.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define TOUCH_BYTE 0xa5

enum discard {
	DISCARD_DONTNEED,
	DISCARD_REMAP,
};

struct refault_result {
	bool mapped;
	bool discarded;
	bool read_ok;
	bool zero_filled;
	bool siblings_unchanged;
	bool pfn_available;
	bool reused;
	long stale;
};

/* Offset of the first byte in @slice after the touched one that is not 0. */
static long stale_offset(const unsigned char *slice)
{
	unsigned long i;

	for (i = 1; i < PROCESS_PAGE_SIZE; i++)
		if (slice[i])
			return i;
	return -1;
}

static bool discard_slice(unsigned char *slice, enum discard how)
{
	void *p;

	if (how == DISCARD_DONTNEED)
		return !madvise(slice, PROCESS_PAGE_SIZE, MADV_DONTNEED);
	if (munmap(slice, PROCESS_PAGE_SIZE))
		return false;
	p = mmap(slice, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	return p == slice;
}

static struct refault_result run_refault(unsigned int victim,
					 unsigned int also, enum discard how,
					bool read_first)
{
	struct refault_result result = { .read_ok = true, .stale = -1 };
	unsigned char *reservation = NULL, *base, *slice;
	uint64_t pfn[P3S_SLICES];
	unsigned int i;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	if (base == MAP_FAILED)
		return result;
	result.mapped = true;
	slice = base + victim * PROCESS_PAGE_SIZE;
	fill_slices(base, 0x31);
	result.discarded = discard_slice(slice, how) &&
		(also == victim || discard_slice(base + also * PROCESS_PAGE_SIZE,
						    how));
	if (!result.discarded)
		goto out;
	if (read_first)
		result.read_ok = FORCE_READ(*slice) == 0;
	slice[0] = TOUCH_BYTE;
	result.stale = stale_offset(slice);
	result.zero_filled = slice[0] == TOUCH_BYTE && result.stale < 0;
	result.siblings_unchanged = true;
	for (i = 0; i < P3S_SLICES; i++) {
		const unsigned char *p = base + i * PROCESS_PAGE_SIZE;

		if (i == victim || i == also)
			continue;
		if (p[0] != 0x31 + i || p[PROCESS_PAGE_SIZE - 1] != 0x31 + i)
			result.siblings_unchanged = false;
	}
	result.pfn_available = p3s_pfn(slice, &pfn[victim]);
	result.reused = result.pfn_available;
	if (result.pfn_available) {
		for (i = 0; i < P3S_SLICES; i++) {
			if (i == victim || i == also)
				continue;
			result.reused &= p3s_pfn(base + i * PROCESS_PAGE_SIZE,
						   &pfn[i]) &&
					 pfn[i] == pfn[victim];
		}
	}
out:
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
	return result;
}

#define REFAULT_CASE(name, victim, also, how, read_first) \
TEST(name##_zero_filled) \
{ \
	struct refault_result r = run_refault(victim, also, how, read_first); \
\
	ASSERT_TRUE(r.mapped); \
	ASSERT_TRUE(r.discarded); \
	EXPECT_TRUE(r.read_ok); \
	EXPECT_TRUE(r.zero_filled); \
	EXPECT_LT(r.stale, 0); \
} \
TEST(name##_preserves_untouched_siblings) \
{ \
	struct refault_result r = run_refault(victim, also, how, read_first); \
\
	ASSERT_TRUE(r.mapped); \
	ASSERT_TRUE(r.discarded); \
	EXPECT_TRUE(r.siblings_unchanged); \
} \
TEST(name##_reuses_native_sibling) \
{ \
	struct refault_result r = run_refault(victim, also, how, read_first); \
\
	ASSERT_TRUE(r.mapped); \
	ASSERT_TRUE(r.discarded); \
	ASSERT_TRUE(r.zero_filled); \
	if (!r.pfn_available) \
		SKIP(return, "PFNs unavailable"); \
	EXPECT_TRUE(r.reused); \
}

REFAULT_CASE(dontneed_slice_0, 0, 0, DISCARD_DONTNEED, false)
REFAULT_CASE(dontneed_slice_1, 1, 1, DISCARD_DONTNEED, false)
REFAULT_CASE(dontneed_slice_2, 2, 2, DISCARD_DONTNEED, false)
REFAULT_CASE(dontneed_slice_3, 3, 3, DISCARD_DONTNEED, false)
REFAULT_CASE(dontneed_two_slices, 1, 2, DISCARD_DONTNEED, false)
REFAULT_CASE(unmap_and_remap_slice, 1, 1, DISCARD_REMAP, false)
REFAULT_CASE(read_then_write_slice, 1, 1, DISCARD_DONTNEED, true)

P3S_HARNESS_4KB_MAIN
