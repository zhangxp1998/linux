// SPDX-License-Identifier: GPL-2.0
/* Keep native trace mmap and the read/splice interfaces for 4KB processes. */
#define _GNU_SOURCE

#include <linux/trace_mmap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define TRACE_PIPE_RAW "/sys/kernel/tracing/per_cpu/cpu0/trace_pipe_raw"

FIXTURE(trace_ring) {
	int fd;
};

FIXTURE_SETUP(trace_ring)
{
	self->fd = p3s_open_fixture(TRACE_PIPE_RAW, O_RDONLY | O_NONBLOCK);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "CPU0 trace ring buffer is unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(trace_ring)
{
	close(self->fd);
}

TEST_F(trace_ring, compat_rejects_metadata_mmap)
{
	void *meta;
	int error;

	if (getpagesize() != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	errno = 0;
	meta = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, 0);
	error = errno;
	EXPECT_EQ(MAP_FAILED, meta);
	EXPECT_EQ(EOPNOTSUPP, error);
	if (meta != MAP_FAILED)
		munmap(meta, PROCESS_PAGE_SIZE);
}

TEST_F(trace_ring, compat_rejects_kernel_page_offset_mmap)
{
	void *data;
	int error;

	if (getpagesize() != PROCESS_PAGE_SIZE)
		SKIP(return, "requires a 4KB process");
	errno = 0;
	data = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, NATIVE_PAGE_SIZE);
	error = errno;
	EXPECT_EQ(MAP_FAILED, data);
	EXPECT_EQ(EOPNOTSUPP, error);
	if (data != MAP_FAILED)
		munmap(data, NATIVE_PAGE_SIZE);
}

TEST_F(trace_ring, read_remains_nonblocking)
{
	char buffer[NATIVE_PAGE_SIZE];
	ssize_t ret;
	int error;

	errno = 0;
	ret = read(self->fd, buffer, sizeof(buffer));
	error = errno;
	EXPECT_TRUE(ret >= 0 || error == EAGAIN);
}

TEST_F(trace_ring, splice_remains_nonblocking)
{
	char buffer[NATIVE_PAGE_SIZE];
	int pipes[2];
	ssize_t ret;
	int error;

	ASSERT_EQ(0, pipe2(pipes, O_CLOEXEC | O_NONBLOCK));
	errno = 0;
	ret = splice(self->fd, NULL, pipes[1], NULL,
		     sizeof(buffer), SPLICE_F_NONBLOCK);
	error = errno;
	EXPECT_TRUE(ret >= 0 || error == EAGAIN);
	close(pipes[0]);
	close(pipes[1]);
}

static bool valid_layout(struct trace_buffer_meta *meta, unsigned long page,
			 unsigned long *meta_len, unsigned long *data_len)
{
	*meta_len = meta->meta_page_size;
	*data_len = (unsigned long)meta->subbuf_size * meta->nr_subbufs;
	return meta->meta_struct_len >= sizeof(*meta) &&
	       meta->meta_struct_len <= *meta_len && *meta_len >= page &&
	       !(*meta_len % page) && meta->subbuf_size &&
	       meta->nr_subbufs && *data_len;
}

TEST_F(trace_ring, native_metadata_describes_kernel_page_ring)
{
	struct trace_buffer_meta *meta;
	unsigned long meta_len, data_len;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	meta = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, 0);
	ASSERT_NE(meta, MAP_FAILED);
	EXPECT_TRUE(valid_layout(meta, NATIVE_PAGE_SIZE,
				 &meta_len, &data_len));
	munmap(meta, NATIVE_PAGE_SIZE);
}

TEST_F(trace_ring, native_page_offset_follows_ring_layout)
{
	struct trace_buffer_meta *meta;
	unsigned long meta_len, data_len;
	void *subpage;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	meta = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, 0);
	ASSERT_NE(meta, MAP_FAILED);
	ASSERT_TRUE(valid_layout(meta, NATIVE_PAGE_SIZE,
				  &meta_len, &data_len));
	subpage = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, NATIVE_PAGE_SIZE);
	EXPECT_EQ(meta_len == NATIVE_PAGE_SIZE,
		  subpage != MAP_FAILED);
	if (subpage != MAP_FAILED)
		munmap(subpage, NATIVE_PAGE_SIZE);
	munmap(meta, NATIVE_PAGE_SIZE);
}

TEST_F(trace_ring, native_data_pages_are_readable)
{
	struct trace_buffer_meta *meta;
	unsigned long meta_len, data_len, offset;
	unsigned char checksum = 0;
	unsigned char *data;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	meta = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, 0);
	ASSERT_NE(meta, MAP_FAILED);
	ASSERT_TRUE(valid_layout(meta, NATIVE_PAGE_SIZE,
				  &meta_len, &data_len));
	data = mmap(NULL, data_len, PROT_READ, MAP_SHARED,
		    self->fd, meta_len);
	ASSERT_NE(data, MAP_FAILED);
	for (offset = 0; offset < data_len; offset += NATIVE_PAGE_SIZE)
		checksum ^= data[offset];
	TH_LOG("mapped data checksum=%u", checksum);
	munmap(data, data_len);
	munmap(meta, NATIVE_PAGE_SIZE);
}

TEST_F(trace_ring, native_reader_ioctl_swaps_subbuffer)
{
	struct trace_buffer_meta *meta;
	unsigned long meta_len, data_len;
	unsigned char *data;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		SKIP(return, "requires native 16KB pages");
	meta = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    self->fd, 0);
	ASSERT_NE(meta, MAP_FAILED);
	ASSERT_TRUE(valid_layout(meta, NATIVE_PAGE_SIZE,
				  &meta_len, &data_len));
	data = mmap(NULL, data_len, PROT_READ, MAP_SHARED,
		    self->fd, meta_len);
	ASSERT_NE(data, MAP_FAILED);
	EXPECT_EQ(0, ioctl(self->fd, TRACE_MMAP_IOCTL_GET_READER));
	munmap(data, data_len);
	munmap(meta, NATIVE_PAGE_SIZE);
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
