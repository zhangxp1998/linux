// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_GUARD_INSTALL / MADV_GUARD_REMOVE operate on individual 4K process
 * pages of a compat process: guarded pages fault, unadvised siblings stay
 * readable and removed guards fault back in as zero pages.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#ifndef MADV_GUARD_INSTALL
#define MADV_GUARD_INSTALL 102
#endif
#ifndef MADV_GUARD_REMOVE
#define MADV_GUARD_REMOVE 103
#endif

#define MAPPING_SIZE	(4 * PROCESS_PAGE_SIZE)

static bool read_faults(const unsigned char *address)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return false;
	if (!child) {
		unsigned char value = *address;

		_exit(value == 0xff ? EXIT_FAILURE : EXIT_SUCCESS);
	}
	if (waitpid(child, &status, 0) != child)
		return false;
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

TEST(p3s_madvise_guard)
{
	unsigned char *mapping, *reservation;
	bool installed;
	bool removed;

	mapping = map_aligned(MAPPING_SIZE, &reservation);
	ASSERT_NE(MAP_FAILED, mapping);
	memset(mapping, 0x5a, MAPPING_SIZE);

	installed = !madvise(mapping + PROCESS_PAGE_SIZE, 3 * PROCESS_PAGE_SIZE,
			     MADV_GUARD_INSTALL);
	EXPECT_TRUE(installed);
	EXPECT_TRUE(mapping[0] == 0x5a);
	for (unsigned int i = 1; i < P3S_SLICES; i++)
		EXPECT_TRUE(read_faults(mapping + i * PROCESS_PAGE_SIZE));

	removed = !madvise(mapping + PROCESS_PAGE_SIZE, 3 * PROCESS_PAGE_SIZE,
			   MADV_GUARD_REMOVE);
	EXPECT_TRUE(removed);
	EXPECT_TRUE(mapping[PROCESS_PAGE_SIZE] == 0 &&
		    mapping[2 * PROCESS_PAGE_SIZE] == 0 &&
			 mapping[3 * PROCESS_PAGE_SIZE] == 0);

	munmap(reservation, MAPPING_SIZE + NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
