// SPDX-License-Identifier: GPL-2.0
/*
 * A forked child of a 4KB process faults the same non-native-aligned
 * private file page as its parent, even after the VMA slab has been seeded
 * with objects carrying a different slice offset.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define TARGET_OFFSET		PROCESS_PAGE_SIZE
#define POISON_OFFSET		(3 * PROCESS_PAGE_SIZE)
#define POISON_MAPPINGS		256
#define POISON_STRIDE		(2 * NATIVE_PAGE_SIZE)
#define FILE_PAGES		4

/* A private file whose pages each carry a distinct byte pattern. */
static int create_backing_file(void)
{
	char path[] = "./fork_filemap_p3s.XXXXXX";
	unsigned char page[PROCESS_PAGE_SIZE];
	int fd = mkstemp(path);
	int i;

	if (fd < 0)
		return -1;
	unlink(path);
	for (i = 0; i < FILE_PAGES; i++) {
		memset(page, 0x40 + i, sizeof(page));
		if (pwrite(fd, page, sizeof(page), i * PROCESS_PAGE_SIZE) !=
		    (ssize_t)sizeof(page)) {
			close(fd);
			return -1;
		}
	}
	return fd;
}

TEST(p3s_fork_filemap)
{
	unsigned char expected[PROCESS_PAGE_SIZE];
	unsigned char *poison_region;
	unsigned char *poison_base;
	size_t poison_region_size;
	unsigned char *mapping;
	struct stat statbuf;
	pid_t child;
	int fd;
	int status;
	int i;

	fd = create_backing_file();
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, fstat(fd, &statbuf));
	ASSERT_GE(statbuf.st_size, (off_t)(FILE_PAGES * PROCESS_PAGE_SIZE));
	ASSERT_EQ(sizeof(expected), pread(fd, expected, sizeof(expected), TARGET_OFFSET));

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd,
		       TARGET_OFFSET);
	ASSERT_NE(MAP_FAILED, mapping);
	EXPECT_TRUE(!memcmp(mapping, expected, sizeof(expected)));

	/*
	 * Fill the VMA slab freelist with objects carrying a different P3S
	 * slice offset.  A fork must initialize every field of the duplicated
	 * VMA rather than depending on stale slab contents.
	 */
	poison_region_size = (POISON_MAPPINGS + 1) * POISON_STRIDE;
	poison_region = mmap(NULL, poison_region_size, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, poison_region);
	poison_base = (unsigned char *)(((uintptr_t)poison_region +
					NATIVE_PAGE_SIZE - 1) &
				       ~(NATIVE_PAGE_SIZE - 1));
	for (i = 0; i < POISON_MAPPINGS; i++) {
		unsigned char *address = poison_base + i * POISON_STRIDE +
					 POISON_OFFSET;
		void *poison;

		poison = mmap(address, PROCESS_PAGE_SIZE, PROT_READ,
			      MAP_PRIVATE | MAP_FIXED, fd, POISON_OFFSET);
		ASSERT_EQ(address, poison);
	}
	for (i = 0; i < POISON_MAPPINGS; i++) {
		unsigned char *address = poison_base + i * POISON_STRIDE;

		ASSERT_EQ(0, munmap(address, NATIVE_PAGE_SIZE));
	}

	child = fork();
	ASSERT_GE(child, 0);
	if (!child)
		_exit(memcmp(mapping, expected, sizeof(expected)) ? 1 : 0);

	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	munmap(mapping, PROCESS_PAGE_SIZE);
	munmap(poison_region, poison_region_size);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
