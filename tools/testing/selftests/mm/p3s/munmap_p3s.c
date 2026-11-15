// SPDX-License-Identifier: GPL-2.0
/*
 * munmap of three 4K process pages inside one native page of a compat
 * process's anonymous mapping removes exactly those pages and leaves the
 * adjacent fourth page mapped with its contents.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define MAPPING_SIZE		(8 * PROCESS_PAGE_SIZE)
#define UNMAP_SIZE		(3 * PROCESS_PAGE_SIZE)

static bool is_mapped(void *address)
{
	unsigned char vector;

	errno = 0;
	return !mincore(address, PROCESS_PAGE_SIZE, &vector);
}

TEST(p3s_munmap)
{
	unsigned char *mapping;
	unsigned char *aligned;
	unsigned int page;
	bool first_three_unmapped = true;

	mapping = mmap(NULL, MAPPING_SIZE + NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	ASSERT_NE(MAP_FAILED, mapping);

	aligned = (unsigned char *)(((uintptr_t)mapping +
				    NATIVE_PAGE_SIZE - 1) &
				   ~(NATIVE_PAGE_SIZE - 1));
	for (page = 0; page < 4; page++)
		aligned[page * PROCESS_PAGE_SIZE] = 0x40 + page;

	EXPECT_TRUE(!munmap(aligned, UNMAP_SIZE));

	for (page = 0; page < 3; page++)
		first_three_unmapped &= !is_mapped(aligned +
						  page * PROCESS_PAGE_SIZE);
	EXPECT_TRUE(first_three_unmapped);

	EXPECT_TRUE(is_mapped(aligned + 3 * PROCESS_PAGE_SIZE) &&
		    aligned[3 * PROCESS_PAGE_SIZE] == 0x43);

	munmap(mapping, MAPPING_SIZE + NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
