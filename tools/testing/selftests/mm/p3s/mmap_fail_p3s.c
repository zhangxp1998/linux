// SPDX-License-Identifier: GPL-2.0
/*
 * A fixture's ->mmap() returns -EIO. The unmap_region() rollback of a
 * failed MAP_FIXED mapping must remove only its 4KB range and preserve
 * the neighbouring slices and their data.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define RESERVE_SIZE	(5 * NATIVE_PAGE_SIZE)

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool range_is_mapped(void *address, size_t length)
{
	unsigned char vec[4];

	return !mincore(address, length, vec);
}

TEST(failed_mmap_preserves_neighbour_slices)
{
	unsigned char *mapping;
	unsigned char *target;
	void *result;
	int fd;

	fd = p3s_open_fixture("/dev/mmap_fail_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "mmap failure fixture unavailable");
	ASSERT_GE(fd, 0);

	mapping = mmap(NULL, RESERVE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, mapping);
	target = (unsigned char *)align_up((uintptr_t)mapping,
					   NATIVE_PAGE_SIZE) +
		 NATIVE_PAGE_SIZE;
	EXPECT_TRUE(!((uintptr_t)target % NATIVE_PAGE_SIZE) &&
		    target - PROCESS_PAGE_SIZE >= mapping &&
		    target + NATIVE_PAGE_SIZE <= mapping + RESERVE_SIZE);

	memset(target - PROCESS_PAGE_SIZE, 0x11, PROCESS_PAGE_SIZE);
	memset(target + PROCESS_PAGE_SIZE, 0x22,
	       NATIVE_PAGE_SIZE - PROCESS_PAGE_SIZE);
	errno = 0;
	result = mmap(target, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, 0);
	EXPECT_TRUE(result == MAP_FAILED && errno == EIO);
	ksft_print_msg("mmap result=%p errno=%d\n", result, errno);

	errno = 0;
	EXPECT_TRUE(!range_is_mapped(target, PROCESS_PAGE_SIZE) &&
		    errno == ENOMEM);
	EXPECT_TRUE(range_is_mapped(target - PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE));
	EXPECT_TRUE(range_is_mapped(target + PROCESS_PAGE_SIZE,
				    NATIVE_PAGE_SIZE - PROCESS_PAGE_SIZE));

	EXPECT_EQ(0x11, target[-1]);
	EXPECT_EQ(0x22, target[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x22, target[NATIVE_PAGE_SIZE - 1]);

	munmap(mapping, RESERVE_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
