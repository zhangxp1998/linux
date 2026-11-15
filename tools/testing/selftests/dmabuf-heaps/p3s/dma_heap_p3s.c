// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define BUFFER_SIZE (4 * NATIVE_PAGE_SIZE)

static unsigned char marker(size_t offset)
{
	return 0x40 + 17 * (offset / PROCESS_PAGE_SIZE) + 37 * offset;
}

static bool check_mapping(const unsigned char *mapping, size_t offset,
			  size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (mapping[i] != marker(offset + i))
			return false;
	}
	return true;
}

FIXTURE(dma_heap) {
	unsigned char *reference;
	int heap_fd;
	int buffer_fd;
};

FIXTURE_SETUP(dma_heap)
{
	struct dma_heap_allocation_data allocation = {
		.len = BUFFER_SIZE,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	size_t i;

	self->heap_fd = p3s_open_fixture("/dev/dma_heap/system", O_RDWR);
	if (self->heap_fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "system DMA heap unavailable");
	ASSERT_GE(self->heap_fd, 0);
	ASSERT_EQ(0, ioctl(self->heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation));
	self->buffer_fd = allocation.fd;
	ASSERT_GE(self->buffer_fd, 0);
	self->reference = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, self->buffer_fd, 0);
	ASSERT_NE(self->reference, MAP_FAILED);
	for (i = 0; i < BUFFER_SIZE; i++)
		self->reference[i] = marker(i);
	ASSERT_TRUE(check_mapping(self->reference, 0, BUFFER_SIZE));
}

FIXTURE_TEARDOWN(dma_heap)
{
	munmap(self->reference, BUFFER_SIZE);
	close(self->buffer_fd);
	close(self->heap_fd);
}

static bool test_range(FIXTURE_DATA(dma_heap) *self, size_t offset,
		       size_t length)
{
	bool reject_offset = p3s_is_compat_process() &&
			     offset % NATIVE_PAGE_SIZE;
	unsigned char *mapping;
	bool matches = false;

	errno = 0;
	mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, self->buffer_fd,
		       offset);
	if (reject_offset)
		matches = mapping == MAP_FAILED && errno == EINVAL;
	else if (mapping != MAP_FAILED)
		matches = check_mapping(mapping, offset, length);
	if (mapping != MAP_FAILED)
		munmap(mapping, length);
	return matches;
}

TEST_F(dma_heap, first_page)
{
	EXPECT_TRUE(test_range(self, 0, getpagesize()));
}

TEST_F(dma_heap, second_native_page)
{
	EXPECT_TRUE(test_range(self, NATIVE_PAGE_SIZE, getpagesize()));
}

TEST_F(dma_heap, page_size_offset)
{
	EXPECT_TRUE(test_range(self, getpagesize(), getpagesize()));
}

TEST_F(dma_heap, two_page_size_offset)
{
	EXPECT_TRUE(test_range(self, 2 * getpagesize(), getpagesize()));
}

TEST_F(dma_heap, three_page_size_offset)
{
	EXPECT_TRUE(test_range(self, 3 * getpagesize(), getpagesize()));
}

TEST_F(dma_heap, range_across_native_page)
{
	size_t page = getpagesize();

	EXPECT_TRUE(test_range(self, NATIVE_PAGE_SIZE - page, 2 * page));
}

TEST_F(dma_heap, range_after_native_page)
{
	size_t page = getpagesize();
	size_t length = page < NATIVE_PAGE_SIZE ? NATIVE_PAGE_SIZE - page : page;

	EXPECT_TRUE(test_range(self, NATIVE_PAGE_SIZE + page, length));
}

TEST_F(dma_heap, final_page)
{
	size_t page = getpagesize();

	EXPECT_TRUE(test_range(self, BUFFER_SIZE - page, page));
}

TEST_F(dma_heap, mapping_past_buffer_start_is_rejected)
{
	size_t page = getpagesize();
	unsigned char *mapping;

	errno = 0;
	mapping = mmap(NULL, page, PROT_READ, MAP_SHARED, self->buffer_fd,
		       BUFFER_SIZE);
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, page);
}

TEST_F(dma_heap, mapping_past_buffer_end_is_rejected)
{
	size_t page = getpagesize();
	unsigned char *mapping;

	errno = 0;
	mapping = mmap(NULL, 2 * page, PROT_READ, MAP_SHARED, self->buffer_fd,
		       BUFFER_SIZE - page);
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, 2 * page);
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
