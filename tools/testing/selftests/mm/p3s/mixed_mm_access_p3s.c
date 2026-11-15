// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat caller accessing a native (16K) target through
 * process_vm_readv/writev, /proc/pid/mem, /proc/pid/pagemap, process_madvise
 * and ptrace sees the target's own page geometry across a native-page
 * boundary.
 */
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define MAPPING_SIZE		(2 * NATIVE_PAGE_SIZE)
#define ADDRESS_TAG		(UINT64_C(0xb4) << 56)

struct child_info {
	uintptr_t address;
	long page_size;
};

static bool buffer_is_value(const unsigned char *buffer, size_t length,
			    unsigned char value)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (buffer[i] != value)
			return false;
	}
	return true;
}

static int exec_native_child(int info_fd, int command_fd)
{
	char command_fd_arg[16];
	char info_fd_arg[16];

	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
	p3s_execl(false, NULL, "--child", info_fd_arg, command_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_child(int info_fd, int command_fd)
{
	unsigned char *mapping;
	struct child_info info;
	char command;
	bool contents_ok;

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	memset(mapping, 0x11, NATIVE_PAGE_SIZE);
	memset(mapping + NATIVE_PAGE_SIZE, 0x22, NATIVE_PAGE_SIZE);
	info.address = (uintptr_t)mapping;
	info.page_size = sysconf(_SC_PAGESIZE);
	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;
	contents_ok = buffer_is_value(mapping, NATIVE_PAGE_SIZE, 0x33) &&
		      buffer_is_value(mapping + NATIVE_PAGE_SIZE,
				      NATIVE_PAGE_SIZE, 0x44);
	munmap(mapping, MAPPING_SIZE);
	return contents_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static ssize_t read_process_mem(pid_t pid, uintptr_t address, void *buffer,
				size_t length)
{
	char path[64];
	int fd;
	ssize_t bytes;

	snprintf(path, sizeof(path), "/proc/%d/mem", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	bytes = pread(fd, buffer, length, address);
	close(fd);
	return bytes;
}

static ssize_t read_process_pagemap(pid_t pid, uintptr_t address,
				    uint64_t entries[2])
{
	char path[64];
	off_t offset = address / NATIVE_PAGE_SIZE * sizeof(entries[0]);
	int fd;
	ssize_t bytes;

	snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	bytes = pread(fd, entries, 2 * sizeof(entries[0]), offset);
	close(fd);
	return bytes;
}

struct access_outcome {
	struct child_info child;
	bool child_ready;
	bool readv_full, readv_data, tagged_readv_data;
	bool proc_mem_full, proc_mem_data;
	bool pagemap_full, pagemap_present;
	bool writev_full;
	bool madvise_unaligned_rejected, madvise_aligned_rounded;
	bool ptrace_attached, ptrace_data;
	bool child_exit_ok;
};

static struct access_outcome observe_remote_access(void)
{
	struct access_outcome outcome = {};
	unsigned char read_buffer[MAPPING_SIZE] = {};
	unsigned char write_buffer[MAPPING_SIZE];
	uint64_t pagemap[2] = {};
	struct iovec local, remote;
	ssize_t result;
	unsigned long word0 = 0, word1 = 0;
	int command_pipe[2] = { -1, -1 };
	int info_pipe[2] = { -1, -1 };
	int child_status, pidfd = -1;
	char command = 1;
	pid_t child = -1;
	bool attached = false;

	if (pipe(info_pipe) || pipe(command_pipe))
		goto out;
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		close(info_pipe[0]);
		close(command_pipe[1]);
		_exit(exec_native_child(info_pipe[1], command_pipe[0]));
	}
	close(info_pipe[1]); info_pipe[1] = -1;
	close(command_pipe[0]); command_pipe[0] = -1;
	outcome.child_ready = read_full(info_pipe[0], &outcome.child,
						 sizeof(outcome.child));
	close(info_pipe[0]); info_pipe[0] = -1;
	if (!outcome.child_ready || !outcome.child.address)
		goto out;
	local = (struct iovec){ read_buffer, sizeof(read_buffer) };
	remote = (struct iovec){ (void *)outcome.child.address,
				   sizeof(read_buffer) };
	result = process_vm_readv(child, &local, 1, &remote, 1, 0);
	outcome.readv_full = result == MAPPING_SIZE;
	outcome.readv_data =
		buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
		buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
				NATIVE_PAGE_SIZE, 0x22);
	memset(read_buffer, 0, sizeof(read_buffer));
	remote.iov_base = (void *)(outcome.child.address | ADDRESS_TAG);
	result = process_vm_readv(child, &local, 1, &remote, 1, 0);
	outcome.tagged_readv_data = result == MAPPING_SIZE &&
		buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
		buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
				NATIVE_PAGE_SIZE, 0x22);
	remote.iov_base = (void *)outcome.child.address;
	memset(read_buffer, 0, sizeof(read_buffer));
	result = read_process_mem(child, outcome.child.address, read_buffer,
				  sizeof(read_buffer));
	outcome.proc_mem_full = result == MAPPING_SIZE;
	outcome.proc_mem_data =
		buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
		buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
				NATIVE_PAGE_SIZE, 0x22);
	result = read_process_pagemap(child, outcome.child.address, pagemap);
	outcome.pagemap_full = result == sizeof(pagemap);
	outcome.pagemap_present = (pagemap[0] & PM_PRESENT) &&
				  (pagemap[1] & PM_PRESENT);
	memset(write_buffer, 0x33, NATIVE_PAGE_SIZE);
	memset(write_buffer + NATIVE_PAGE_SIZE, 0x44, NATIVE_PAGE_SIZE);
	local = (struct iovec){ write_buffer, sizeof(write_buffer) };
	result = process_vm_writev(child, &local, 1, &remote, 1, 0);
	outcome.writev_full = result == MAPPING_SIZE;
	pidfd = syscall(__NR_pidfd_open, child, 0);
	if (pidfd < 0)
		goto out;
	remote.iov_base = (void *)(outcome.child.address + PROCESS_PAGE_SIZE);
	remote.iov_len = PROCESS_PAGE_SIZE;
	errno = 0;
	result = syscall(__NR_process_madvise, pidfd, &remote, 1,
			 MADV_WILLNEED, 0);
	outcome.madvise_unaligned_rejected = result == -1 && errno == EINVAL;
	remote.iov_base = (void *)outcome.child.address;
	errno = 0;
	result = syscall(__NR_process_madvise, pidfd, &remote, 1,
			 MADV_WILLNEED, 0);
	outcome.madvise_aligned_rounded = result == PROCESS_PAGE_SIZE;
	if (!ptrace(PTRACE_ATTACH, child, NULL, NULL) &&
	    waitpid(child, &child_status, 0) == child &&
	    WIFSTOPPED(child_status))
		attached = true;
	outcome.ptrace_attached = attached;
	if (attached) {
		errno = 0;
		word0 = ptrace(PTRACE_PEEKDATA, child,
				       (void *)(outcome.child.address +
						 PROCESS_PAGE_SIZE), NULL);
		if (errno)
			attached = false;
		errno = 0;
		word1 = ptrace(PTRACE_PEEKDATA, child,
				       (void *)(outcome.child.address +
						 NATIVE_PAGE_SIZE), NULL);
		if (errno)
			attached = false;
		ptrace(PTRACE_DETACH, child, NULL, NULL);
	}
	outcome.ptrace_data = attached &&
		word0 == 0x3333333333333333UL &&
		word1 == 0x4444444444444444UL;
	if (write_full(command_pipe[1], &command, sizeof(command)) &&
	    waitpid(child, &child_status, 0) == child) {
		outcome.child_exit_ok = WIFEXITED(child_status) &&
					WEXITSTATUS(child_status) == EXIT_SUCCESS;
		child = -1;
	}
 out:
	if (command_pipe[1] >= 0)
		close(command_pipe[1]);
	if (command_pipe[0] >= 0)
		close(command_pipe[0]);
	if (info_pipe[1] >= 0)
		close(info_pipe[1]);
	if (info_pipe[0] >= 0)
		close(info_pipe[0]);
	if (child > 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	if (pidfd >= 0)
		close(pidfd);
	return outcome;
}

FIXTURE(remote_access) {
	struct access_outcome result;
};

FIXTURE_SETUP(remote_access)
{
	self->result = observe_remote_access();
}

FIXTURE_TEARDOWN(remote_access)
{
}

TEST_F(remote_access, compat_caller_targets_native_process)
{
	ASSERT_TRUE(self->result.child_ready);
	EXPECT_EQ(PROCESS_PAGE_SIZE, sysconf(_SC_PAGESIZE));
	EXPECT_EQ(NATIVE_PAGE_SIZE, self->result.child.page_size);
	EXPECT_EQ(0, self->result.child.address & (NATIVE_PAGE_SIZE - 1));
}

TEST_F(remote_access, process_vm_readv_crosses_native_boundary)
{
	EXPECT_TRUE(self->result.readv_full);
}

TEST_F(remote_access, process_vm_readv_preserves_both_pages)
{
	EXPECT_TRUE(self->result.readv_data);
}

TEST_F(remote_access, process_vm_readv_accepts_tagged_native_address)
{
	EXPECT_TRUE(self->result.tagged_readv_data);
}

TEST_F(remote_access, proc_mem_crosses_native_boundary)
{
	EXPECT_TRUE(self->result.proc_mem_full);
}

TEST_F(remote_access, proc_mem_uses_target_geometry)
{
	EXPECT_TRUE(self->result.proc_mem_data);
}

TEST_F(remote_access, pagemap_accepts_native_target_offset)
{
	EXPECT_TRUE(self->result.pagemap_full);
}

TEST_F(remote_access, pagemap_reports_both_native_pages)
{
	EXPECT_TRUE(self->result.pagemap_present);
}

TEST_F(remote_access, process_vm_writev_crosses_native_boundary)
{
	EXPECT_TRUE(self->result.writev_full);
}

TEST_F(remote_access, process_madvise_rejects_unaligned_native_start)
{
	EXPECT_TRUE(self->result.madvise_unaligned_rejected);
}

TEST_F(remote_access, process_madvise_rounds_using_target_geometry)
{
	EXPECT_TRUE(self->result.madvise_aligned_rounded);
}

TEST_F(remote_access, ptrace_attaches_across_page_size_modes)
{
	EXPECT_TRUE(self->result.ptrace_attached);
}

TEST_F(remote_access, ptrace_reads_slice_and_native_boundary)
{
	EXPECT_TRUE(self->result.ptrace_data);
}

TEST_F(remote_access, native_target_observes_remote_writes)
{
	EXPECT_TRUE(self->result.child_exit_ok);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		p3s_require_native_compat();
	if (!mode)
		exec_compat(argv[0], "--parent", NULL);
	if (argc == 2 && !strcmp(mode, "--parent"))
		return p3s_harness_run();
	if (argc == 4 && !strcmp(mode, "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return EXIT_FAILURE;
}
