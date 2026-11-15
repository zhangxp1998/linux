// SPDX-License-Identifier: GPL-2.0
/* videobuf2 plane mmap size and slice-cookie bounds in 4KB processes. */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_harness.h"

#define VALID_MAP_SIZE (2 * PROCESS_PAGE_SIZE)

FIXTURE(vb2_mmap) {
	int fd;
};

FIXTURE_SETUP(vb2_mmap)
{
	self->fd = p3s_open_fixture("/dev/vb2_mmap_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "videobuf2 mmap fixture unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(vb2_mmap)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(vb2_mmap, plane_maps_as_two_process_pages)
{
	void *mapping;

	mapping = mmap(NULL, VALID_MAP_SIZE, PROT_READ,
		       MAP_SHARED, self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	munmap(mapping, VALID_MAP_SIZE);
}

TEST_F(vb2_mmap, rejects_nonexistent_plane_cookie)
{
	void *mapping;
	int saved_errno;

	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, PROCESS_PAGE_SIZE);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

TEST_F(vb2_mmap, rejects_mapping_past_plane_process_pages)
{
	void *mapping;
	int saved_errno;

	errno = 0;
	mapping = mmap(NULL, VALID_MAP_SIZE + PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED, self->fd, 0);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, VALID_MAP_SIZE + PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
