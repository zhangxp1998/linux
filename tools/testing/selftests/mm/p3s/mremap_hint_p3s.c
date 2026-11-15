// SPDX-License-Identifier: GPL-2.0
/*
 * mremap(MREMAP_MAYMOVE | MREMAP_DONTUNMAP) of a 4K compat mapping honors a
 * free hint only at the source slice, preserves every process page
 * and leaves the source zero-filled.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

#define TEST_LENGTH (4 * PROCESS_PAGE_SIZE)
#define RESERVE_LENGTH (TEST_LENGTH + 2 * NATIVE_PAGE_SIZE)
#define HINT_WINDOW (TEST_LENGTH + NATIVE_PAGE_SIZE)

static unsigned char *native_aligned(void *address)
{
	return (unsigned char *)(((uintptr_t)address + NATIVE_PAGE_SIZE - 1) &
				  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
}

TEST(p3s_mremap_hint)
{
	unsigned char *source_reservation, *hint_reservation;
	unsigned char *source, *hint;
	unsigned char *moved;
	bool moved_data_ok = true;
	bool source_zeroed = true;
	unsigned int i;

	source_reservation = mmap(NULL, RESERVE_LENGTH, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(source_reservation, MAP_FAILED);
	source = native_aligned(source_reservation) + PROCESS_PAGE_SIZE;
	ASSERT_EQ(source, mmap(source, TEST_LENGTH, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
	for (i = 0; i < TEST_LENGTH; i++)
		source[i] = i / PROCESS_PAGE_SIZE + 0x31;

	/* Leave a free, native-aligned window large enough for the extra page. */
	hint_reservation = mmap(NULL, HINT_WINDOW + 2 * NATIVE_PAGE_SIZE,
				PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(hint_reservation, MAP_FAILED);
	hint = native_aligned(hint_reservation);
	ASSERT_EQ(0, munmap(hint, HINT_WINDOW));

	moved = mremap(source, TEST_LENGTH, TEST_LENGTH,
		       MREMAP_MAYMOVE | MREMAP_DONTUNMAP, hint);
	ASSERT_NE(moved, MAP_FAILED);
	EXPECT_TRUE(moved == hint + PROCESS_PAGE_SIZE);

	for (i = 0; i < TEST_LENGTH; i++) {
		if (moved[i] != (unsigned char)(i / PROCESS_PAGE_SIZE + 0x31))
			moved_data_ok = false;
		if (source[i])
			source_zeroed = false;
	}
	EXPECT_TRUE(moved_data_ok);
	EXPECT_TRUE(source_zeroed);

	munmap(moved, TEST_LENGTH);
	munmap(source_reservation, RESERVE_LENGTH);
	munmap(hint_reservation, HINT_WINDOW + 2 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
