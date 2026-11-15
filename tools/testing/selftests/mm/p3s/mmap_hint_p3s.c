// SPDX-License-Identifier: GPL-2.0
/*
 * Default mmap placements of a 4K compat process are native-page aligned,
 * while an explicit hint at a free address that is only 4K aligned is
 * honored and the resulting mapping is usable.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_harness.h"
#include "../vm_util.h"

#define RESERVE_SIZE	(4 * PROCESS_PAGE_SIZE)
#define ALIGNMENT_TRIALS 32

static bool default_mmaps_are_native_aligned(void)
{
	void *mappings[ALIGNMENT_TRIALS];
	unsigned int mapped = 0;
	bool passed = true;

	for (mapped = 0; mapped < ALIGNMENT_TRIALS; mapped++) {
		mappings[mapped] = mmap(NULL, PROCESS_PAGE_SIZE,
					PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mappings[mapped] == MAP_FAILED) {
			passed = false;
			break;
		}
		if ((uintptr_t)mappings[mapped] & (NATIVE_PAGE_SIZE - 1))
			passed = false;
	}
	while (mapped)
		munmap(mappings[--mapped], PROCESS_PAGE_SIZE);

	return passed;
}

TEST(default_mmaps_are_native_page_aligned)
{
	EXPECT_TRUE(default_mmaps_are_native_aligned());
}

TEST(free_4k_only_hint_is_honored_and_usable)
{
	unsigned char *reservation;
	unsigned char *mapping;
	unsigned char *hint;

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	hint = reservation + PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, munmap(reservation, RESERVE_SIZE));
	ASSERT_NE(0UL, (uintptr_t)hint & (NATIVE_PAGE_SIZE - 1));

	mapping = mmap(hint, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	EXPECT_EQ(hint, mapping);
	if (mapping != MAP_FAILED) {
		mapping[0] = 0x5a;
		EXPECT_EQ(0x5a, FORCE_READ(mapping[0]));
		munmap(mapping, PROCESS_PAGE_SIZE);
	}
}

P3S_HARNESS_4KB_MAIN
