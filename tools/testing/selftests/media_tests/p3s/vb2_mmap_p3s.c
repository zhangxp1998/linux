// SPDX-License-Identifier: GPL-2.0
/* videobuf2 plane mmap and exported DMA-BUF geometry in P3S and native modes. */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define TEST_PLANE_SIZE (6 * 1024UL)
#define VALID_MAP_SIZE (2 * PROCESS_PAGE_SIZE)
#define VB2_MMAP_P3S_EXPBUF _IO('v', 0x70)

FIXTURE(vb2_mmap) {
	int fd;
	int dmabuf_fd;
	size_t page_size;
	size_t valid_map_size;
};

FIXTURE_SETUP(vb2_mmap)
{
	self->page_size = getpagesize();
	ASSERT_TRUE(self->page_size == PROCESS_PAGE_SIZE ||
		    self->page_size == NATIVE_PAGE_SIZE);
	self->valid_map_size = (TEST_PLANE_SIZE + self->page_size - 1) &
				     ~(self->page_size - 1);
	self->fd = p3s_open_fixture("/dev/vb2_mmap_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "videobuf2 mmap fixture unavailable");
	ASSERT_GE(self->fd, 0);
	self->dmabuf_fd = ioctl(self->fd, VB2_MMAP_P3S_EXPBUF);
	ASSERT_GE(self->dmabuf_fd, 0);
}

FIXTURE_TEARDOWN(vb2_mmap)
{
	close(self->dmabuf_fd);
	close(self->fd);
}

TEST_F(vb2_mmap, compat_plane_maps_as_two_process_pages)
{
	void *mapping;

	if (self->page_size != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	mapping = mmap(NULL, VALID_MAP_SIZE, PROT_READ,
		       MAP_SHARED, self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	munmap(mapping, VALID_MAP_SIZE);
}

TEST_F(vb2_mmap, compat_rejects_nonexistent_plane_cookie)
{
	void *mapping;
	int saved_errno;

	if (self->page_size != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, PROCESS_PAGE_SIZE);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

TEST_F(vb2_mmap, compat_dmabuf_keeps_native_allocation)
{
	struct stat st = {};

	if (self->page_size != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	ASSERT_EQ(0, fstat(self->dmabuf_fd, &st));
	EXPECT_EQ(NATIVE_PAGE_SIZE, st.st_size);
}

TEST_F(vb2_mmap, compat_dmabuf_maps_final_process_page)
{
	void *mapping;

	if (self->page_size != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	mapping = mmap(NULL, VALID_MAP_SIZE, PROT_READ,
		       MAP_SHARED, self->dmabuf_fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	munmap(mapping, VALID_MAP_SIZE);
}

TEST_F(vb2_mmap, compat_dmabuf_rejects_past_allocation)
{
	void *mapping;
	int saved_errno;
	size_t length = NATIVE_PAGE_SIZE + PROCESS_PAGE_SIZE;

	if (self->page_size != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	errno = 0;
	mapping = mmap(NULL, length, PROT_READ, MAP_SHARED,
		       self->dmabuf_fd, 0);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, length);
}

TEST_F(vb2_mmap, native_dmabuf_fits_process_page_range)
{
	struct stat st = {};

	if (self->page_size != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	ASSERT_EQ(0, fstat(self->dmabuf_fd, &st));
	EXPECT_GE(st.st_size, TEST_PLANE_SIZE);
	EXPECT_LE(st.st_size, self->valid_map_size);
}

TEST_F(vb2_mmap, native_dmabuf_maps_final_process_page)
{
	void *mapping;

	if (self->page_size != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	mapping = mmap(NULL, self->valid_map_size, PROT_READ,
		       MAP_SHARED, self->dmabuf_fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	munmap(mapping, self->valid_map_size);
}

TEST_F(vb2_mmap, native_dmabuf_rejects_one_page_overrun)
{
	void *mapping;
	int saved_errno;
	size_t length = self->valid_map_size + self->page_size;

	if (self->page_size != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	errno = 0;
	mapping = mmap(NULL, length, PROT_READ, MAP_SHARED,
		       self->dmabuf_fd, 0);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, length);
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
