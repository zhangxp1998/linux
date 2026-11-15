// SPDX-License-Identifier: GPL-2.0
/* 4KB processes refuse trace_pipe_raw mmap while keeping read and splice. */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_harness.h"

FIXTURE(trace_ring) {
	char instance[128];
	bool instance_created;
	int fd;
};

FIXTURE_SETUP(trace_ring)
{
	static const char * const roots[] = {
		"/sys/kernel/tracing",
		"/sys/kernel/debug/tracing",
	};
	char path[256];
	unsigned int i;

	self->fd = -1;
	self->instance_created = false;
	for (i = 0; i < ARRAY_SIZE(roots); i++) {
		snprintf(path, sizeof(path), "%s/instances", roots[i]);
		if (access(path, W_OK))
			continue;
		snprintf(self->instance, sizeof(self->instance),
			 "%s/instances/p3s_rb_%d", roots[i], getpid());
		if (!mkdir(self->instance, 0700)) {
			self->instance_created = true;
			break;
		}
		if (errno != EACCES && errno != EPERM && errno != ENOENT &&
		    errno != EROFS && errno != EOPNOTSUPP)
			ASSERT_EQ(0, -1) TH_LOG("mkdir %s failed: %s",
						self->instance,
						strerror(errno));
	}
	if (!self->instance_created)
		SKIP(return, "tracefs instances unavailable");

	snprintf(path, sizeof(path), "%s/per_cpu/cpu0/trace_pipe_raw",
		 self->instance);
	self->fd = p3s_open_fixture(path, O_RDONLY | O_NONBLOCK);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "CPU0 trace ring buffer is unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(trace_ring)
{
	if (self->fd >= 0)
		close(self->fd);
	if (self->instance_created)
		EXPECT_EQ(0, rmdir(self->instance));
}

TEST_F(trace_ring, four_kb_rejects_metadata_mmap)
{
	static const size_t lengths[] = { PROCESS_PAGE_SIZE, NATIVE_PAGE_SIZE };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lengths); i++) {
		void *meta;
		int error;

		errno = 0;
		meta = mmap(NULL, lengths[i], PROT_READ, MAP_SHARED,
			    self->fd, 0);
		error = errno;
		EXPECT_EQ(MAP_FAILED, meta);
		EXPECT_EQ(EOPNOTSUPP, error);
		if (meta != MAP_FAILED)
			munmap(meta, lengths[i]);
	}
}

TEST_F(trace_ring, four_kb_rejects_kernel_page_offset_mmap)
{
	void *data;
	int error;

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

P3S_HARNESS_4KB_MAIN
