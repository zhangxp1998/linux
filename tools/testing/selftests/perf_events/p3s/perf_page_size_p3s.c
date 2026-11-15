// SPDX-License-Identifier: GPL-2.0
/* A native perf reader observes byte offsets and page sizes of a 4KB child. */
#define _GNU_SOURCE
#include <linux/memfd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

static void copy_ring(void *to, const void *data, size_t size,
		      uint64_t position, size_t length)
{
	size_t offset = position % size;
	size_t first = size - offset;

	if (first > length)
		first = length;
	memcpy(to, data + offset, first);
	if (first < length)
		memcpy(to + first, data, length - first);
}

static int child(void)
{
	unsigned char *memory;
	char command;
	int fd;
	void *text;

	if (!p3s_is_4kb_process())
		return KSFT_SKIP;
	memory = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED ||
	    write(STDOUT_FILENO, &memory, sizeof(memory)) != sizeof(memory) ||
	    read(STDIN_FILENO, &command, 1) != 1)
		return KSFT_FAIL;
	fd = memfd_create("perf-observer-p3s", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE))
		return KSFT_FAIL;
	text = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC,
		    MAP_PRIVATE, fd, PROCESS_PAGE_SIZE);
	if (text == MAP_FAILED)
		return KSFT_FAIL;
	(void)__atomic_load_n(memory, __ATOMIC_RELAXED);
	munmap(text, PROCESS_PAGE_SIZE);
	munmap(memory, PROCESS_PAGE_SIZE);
	close(fd);
	return KSFT_PASS;
}

struct perf_outcome {
	bool child_unavailable;
	bool perf_unavailable;
	int perf_errno;
	bool child_started;
	bool ring_mapped;
	bool child_ok;
	bool sample;
	bool record;
	uint64_t sizes[3];
	uint64_t file_offset;
};

static struct perf_outcome observe_perf(void)
{
	struct perf_event_attr attr = {
		.size = sizeof(attr), .type = PERF_TYPE_SOFTWARE,
		.config = PERF_COUNT_SW_PAGE_FAULTS_MIN, .sample_period = 1,
		.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_DATA_PAGE_SIZE |
			       PERF_SAMPLE_CODE_PAGE_SIZE,
		.disabled = 1, .exclude_kernel = 1, .exclude_hv = 1,
		.mmap = 1, .mmap2 = 1, .mmap_data = 1,
	};
	struct perf_outcome outcome = { .file_offset = UINT64_MAX };
	struct perf_event_mmap_page *metadata = MAP_FAILED;
	unsigned char *data;
	uint64_t address = 0, head, position = 0;
	int ready[2] = { -1, -1 }, go[2] = { -1, -1 }, fd = -1, status;
	size_t ring_size = 17 * NATIVE_PAGE_SIZE;
	pid_t pid = -1;

	if (pipe(ready) || pipe(go))
		goto out;
	pid = fork();
	if (!pid) {
		close(ready[0]);
		close(go[1]);
		if (dup2(ready[1], STDOUT_FILENO) < 0 ||
		    dup2(go[0], STDIN_FILENO) < 0)
			_exit(KSFT_FAIL);
		close(ready[1]);
		close(go[0]);
		p3s_execl(true, NULL, "--child", NULL);
		_exit(KSFT_FAIL);
	}
	close(ready[1]);
	ready[1] = -1;
	close(go[0]);
	go[0] = -1;
	if (pid < 0 || read(ready[0], &address, sizeof(address)) != sizeof(address))
		goto out;
	outcome.child_started = true;
	close(ready[0]);
	ready[0] = -1;
	fd = syscall(__NR_perf_event_open, &attr, pid, -1, -1, 0);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM || errno == ENOENT ||
		    errno == ENOSYS) {
			outcome.perf_unavailable = true;
			outcome.perf_errno = errno;
			close(go[1]);
			go[1] = -1;
			waitpid(pid, &status, 0);
			pid = -1;
		}
		goto out;
	}
	metadata = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (metadata == MAP_FAILED) {
		if (errno == EACCES || errno == EPERM || errno == ENOMEM) {
			outcome.perf_unavailable = true;
			outcome.perf_errno = errno;
			close(go[1]);
			go[1] = -1;
			waitpid(pid, &status, 0);
			pid = -1;
		}
		goto out;
	}
	outcome.ring_mapped = true;
	if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))
		goto out;
	if (write(go[1], "g", 1) != 1)
		goto out;
	close(go[1]);
	go[1] = -1;
	if (waitpid(pid, &status, 0) != pid)
		goto out;
	pid = -1;
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	outcome.child_ok = WIFEXITED(status) && !WEXITSTATUS(status);
	data = (unsigned char *)metadata + metadata->data_offset;
	head = __atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE);
	while (position < head) {
		struct perf_event_header h;

		copy_ring(&h, data, metadata->data_size, position, sizeof(h));
		if (h.size < sizeof(h) || h.size > metadata->data_size)
			break;
		if (h.type == PERF_RECORD_SAMPLE &&
		    h.size >= sizeof(h) + sizeof(outcome.sizes)) {
			uint64_t sizes[3];

			copy_ring(sizes, data, metadata->data_size,
				  position + sizeof(h), sizeof(sizes));
			if (sizes[0] == address) {
				memcpy(outcome.sizes, sizes, sizeof(sizes));
				outcome.sample = true;
			}
		}
		if (h.type == PERF_RECORD_MMAP2 && h.size > 72) {
			char *record = malloc(h.size);

			if (!record)
				break;
			copy_ring(record, data, metadata->data_size, position,
				  h.size);
			if (memmem(record + 72, h.size - 72,
				   "perf-observer-p3s", 17)) {
				memcpy(&outcome.file_offset, record + 32,
				       sizeof(outcome.file_offset));
				outcome.record = true;
			}
			free(record);
		}
		position += h.size;
	}
out:
	if (go[1] >= 0)
		close(go[1]);
	if (go[0] >= 0)
		close(go[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	if (ready[0] >= 0)
		close(ready[0]);
	if (pid > 0 && waitpid(pid, &status, 0) == pid &&
	    !outcome.child_started && WIFEXITED(status) &&
	    WEXITSTATUS(status) == KSFT_SKIP)
		outcome.child_unavailable = true;
	if (metadata != MAP_FAILED)
		munmap(metadata, ring_size);
	if (fd >= 0)
		close(fd);
	return outcome;
}

FIXTURE(perf_reader) {
	struct perf_outcome result;
};

FIXTURE_SETUP(perf_reader)
{
	self->result = observe_perf();
	if (self->result.child_unavailable)
		SKIP(return, "4KB child unavailable");
	if (self->result.perf_unavailable)
		SKIP(return, "perf_event_open unavailable: %s",
		     strerror(self->result.perf_errno));
}

FIXTURE_TEARDOWN(perf_reader)
{
}

TEST_F(perf_reader, native_reader_starts_4k_child)
{
	EXPECT_TRUE(self->result.child_started);
}

TEST_F(perf_reader, native_reader_maps_perf_ring)
{
	EXPECT_TRUE(self->result.ring_mapped);
}

TEST_F(perf_reader, four_kb_child_maps_file_slice_and_faults)
{
	EXPECT_TRUE(self->result.child_ok);
}

TEST_F(perf_reader, capture_minor_fault_sample)
{
	EXPECT_TRUE(self->result.sample);
}

TEST_F(perf_reader, sampled_data_page_size_is_4k)
{
	ASSERT_TRUE(self->result.sample);
	EXPECT_EQ(PROCESS_PAGE_SIZE, self->result.sizes[1]);
}

TEST_F(perf_reader, sampled_code_page_size_is_4k)
{
	ASSERT_TRUE(self->result.sample);
	EXPECT_EQ(PROCESS_PAGE_SIZE, self->result.sizes[2]);
}

TEST_F(perf_reader, mmap2_record_keeps_file_slice_offset)
{
	EXPECT_TRUE(self->result.record);
	EXPECT_EQ(PROCESS_PAGE_SIZE, self->result.file_offset);
}

static int child_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--child"))
		return child();
	return -1;
}

P3S_HARNESS_4KB_MAIN
