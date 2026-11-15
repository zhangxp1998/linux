// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process can still read() /sys/kernel/btf/vmlinux, but mmap() returns
 * EOPNOTSUPP because a trailing 4KB slice check would not bound the mapping.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_harness.h"

#define BTF_PATH	"/sys/kernel/btf/vmlinux"

TEST(four_kb_btf_read_works_but_mmap_is_unsupported)
{
	unsigned char first;
	struct stat st;
	void *map;
	int fd, err;

	fd = open(BTF_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = errno;
		if (p3s_fixture_unavailable(err))
			SKIP(return, "%s unavailable: %s", BTF_PATH,
			     strerror(err));
		ASSERT_GE(fd, 0);
	}
	if (fstat(fd, &st) || st.st_size <= 0 ||
	    pread(fd, &first, 1, 0) != 1) {
		close(fd);
		ASSERT_TRUE(false);
	}
	errno = 0;
	map = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	err = errno;
	EXPECT_EQ(MAP_FAILED, map);
	EXPECT_EQ(EOPNOTSUPP, err);
	if (map != MAP_FAILED)
		munmap(map, PROCESS_PAGE_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
