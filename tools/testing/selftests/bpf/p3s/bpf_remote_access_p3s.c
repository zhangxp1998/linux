// SPDX-License-Identifier: GPL-2.0
/* Native BPF producer mappings retain remote byte access. */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include "p3s_harness.h"

static const uint64_t marker = 0x1234567887654321ULL;

FIXTURE(bpf_producer) {
	uint64_t *producer;
	int fd;
};

FIXTURE_SETUP(bpf_producer)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_USER_RINGBUF,
		.max_entries = NATIVE_PAGE_SIZE,
	};

	if (p3s_is_compat_process())
		SKIP(return, "BPF map mmap is native-only; use --native");
	self->fd = syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
	if (self->fd < 0) {
		if (errno == EPERM || errno == EACCES)
			SKIP(return, "BPF map creation unavailable: %s",
			     strerror(errno));
		ASSERT_GE(self->fd, 0);
	}
	self->producer = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
			      MAP_SHARED, self->fd, getpagesize());
	ASSERT_NE(self->producer, MAP_FAILED);
	*self->producer = marker;
}

FIXTURE_TEARDOWN(bpf_producer)
{
	munmap(self->producer, getpagesize());
	close(self->fd);
}

TEST_F(bpf_producer, mapped_pointer_control)
{
	EXPECT_EQ(marker, *self->producer);
}

TEST_F(bpf_producer, process_vm_readv_reads_slice)
{
	uint64_t value = 0;
	struct iovec local = { .iov_base = &value, .iov_len = sizeof(value) };
	struct iovec remote = { .iov_base = self->producer,
				.iov_len = sizeof(value) };
	ssize_t bytes;

	bytes = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	EXPECT_EQ(sizeof(value), bytes);
	EXPECT_EQ(marker, value);
}

TEST_F(bpf_producer, process_vm_writev_writes_slice)
{
	uint64_t value = marker + 1;
	struct iovec local = { .iov_base = &value, .iov_len = sizeof(value) };
	struct iovec remote = { .iov_base = self->producer,
				.iov_len = sizeof(value) };
	ssize_t bytes;

	bytes = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
	EXPECT_EQ(sizeof(value), bytes);
	EXPECT_EQ(value, *self->producer);
}

TEST_F(bpf_producer, proc_self_mem_reads_slice)
{
	uint64_t value = 0;
	ssize_t bytes;
	int mem = open("/proc/self/mem", O_RDWR);

	if (mem < 0)
		SKIP(return, "proc/self/mem unavailable: %s", strerror(errno));
	bytes = pread(mem, &value, sizeof(value), (uintptr_t)self->producer);
	EXPECT_EQ(sizeof(value), bytes);
	EXPECT_EQ(marker, value);
	close(mem);
}

TEST_F(bpf_producer, proc_self_mem_writes_slice)
{
	uint64_t value = marker + 2;
	ssize_t bytes;
	int mem = open("/proc/self/mem", O_RDWR);

	if (mem < 0)
		SKIP(return, "proc/self/mem unavailable: %s", strerror(errno));
	bytes = pwrite(mem, &value, sizeof(value), (uintptr_t)self->producer);
	EXPECT_EQ(sizeof(value), bytes);
	EXPECT_EQ(value, *self->producer);
	close(mem);
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
