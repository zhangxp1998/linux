// SPDX-License-Identifier: GPL-2.0
/* Public page-size contract on native and P3S processes. */
#define _GNU_SOURCE

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

TEST(reported_page_size_is_consistent)
{
	const size_t page = getpagesize();

	EXPECT_TRUE(page == PROCESS_PAGE_SIZE || page == NATIVE_PAGE_SIZE);
	EXPECT_EQ((long)page, sysconf(_SC_PAGESIZE));
	EXPECT_EQ(page, getauxval(AT_PAGESZ));
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
	unsigned char *mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_EQ(0, mprotect(mapping + page, page, PROT_NONE));
	errno = 0;
	EXPECT_EQ(-1, mprotect(mapping + page / 2, page, PROT_NONE));
	EXPECT_EQ(EINVAL, errno);
	munmap(mapping, 2 * page);
}

TEST(munmap_removes_exactly_one_reported_page)
{
	const size_t page = getpagesize();
	unsigned char *mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char residency;

	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 0x69;
	ASSERT_EQ(0, munmap(mapping + page, page));
	EXPECT_EQ(0x69, mapping[0]);
	errno = 0;
	EXPECT_EQ(-1, mincore(mapping + page, page, &residency));
	EXPECT_EQ(ENOMEM, errno);
	munmap(mapping, page);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE) {
			ksft_print_header();
			ksft_exit_skip("native exec does not use 16K pages\n");
		}
		return p3s_harness_run();
	}
	return p3s_harness_main(argc, argv);
}
