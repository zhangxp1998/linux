// SPDX-License-Identifier: GPL-2.0
/* Public page-size contract on native and P3S processes. */
#define _GNU_SOURCE

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"

TEST(reported_page_size_is_consistent)
{
	const size_t expected = p3s_is_4kb_process() ?
		PROCESS_PAGE_SIZE : NATIVE_PAGE_SIZE;

	EXPECT_EQ(expected, getpagesize());
	EXPECT_EQ(expected, sysconf(_SC_PAGESIZE));
	EXPECT_EQ(expected, getauxval(AT_PAGESZ));
}

static int marked_memfd(size_t page)
{
	unsigned char marker = 0x69;
	int fd = syscall(SYS_memfd_create, "page-size-contract", 0);

	if (fd < 0 || ftruncate(fd, 3 * page) ||
	    pwrite(fd, &marker, 1, page) != 1) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

TEST(mmap_file_offset_uses_reported_page_size)
{
	const size_t page = getpagesize();
	void *file_map;
	void *bad_map;
	int fd = marked_memfd(page);

	ASSERT_GE(fd, 0);
	file_map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
	ASSERT_NE(file_map, MAP_FAILED);
	EXPECT_EQ(0x69, *(unsigned char *)file_map);
	if (file_map != MAP_FAILED)
		munmap(file_map, page);
	errno = 0;
	bad_map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page / 2);
	EXPECT_EQ(MAP_FAILED, bad_map);
	EXPECT_EQ(EINVAL, errno);
	if (bad_map != MAP_FAILED)
		munmap(bad_map, page);
	close(fd);
}

TEST(mprotect_uses_reported_page_boundaries)
{
	const size_t page = getpagesize();
	unsigned char *mapping = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int fds[2];

	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 0x69;
	mapping[page] = 0x96;
	mapping[2 * page] = 0xa5;
	ASSERT_EQ(0, mprotect(mapping + page, page, PROT_NONE));
	EXPECT_EQ(0x69, mapping[0]);
	mapping[0] = 0x5a;
	EXPECT_EQ(0x5a, mapping[0]);
	EXPECT_EQ(0xa5, mapping[2 * page]);
	mapping[2 * page] = 0x5b;
	EXPECT_EQ(0x5b, mapping[2 * page]);
	ASSERT_EQ(0, pipe(fds));
	errno = 0;
	EXPECT_EQ(-1, write(fds[1], mapping + page, 1));
	EXPECT_EQ(EFAULT, errno);
	close(fds[0]);
	close(fds[1]);
	errno = 0;
	EXPECT_EQ(-1, mprotect(mapping + page / 2, page, PROT_NONE));
	EXPECT_EQ(EINVAL, errno);
	munmap(mapping, 3 * page);
}

TEST(munmap_removes_exactly_one_reported_page)
{
	const size_t page = getpagesize();
	unsigned char *mapping = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char residency[P3S_SLICES];

	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 0x69;
	mapping[2 * page] = 0xa5;
	ASSERT_EQ(0, munmap(mapping + page, page));
	EXPECT_EQ(0x69, mapping[0]);
	EXPECT_EQ(0xa5, mapping[2 * page]);
	errno = 0;
	EXPECT_EQ(-1, mincore(mapping + page, page, residency));
	EXPECT_EQ(ENOMEM, errno);
	munmap(mapping, page);
	munmap(mapping + 2 * page, page);
}

P3S_HARNESS_4KB_MAIN
