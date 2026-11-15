// SPDX-License-Identifier: GPL-2.0
/* A 4KB caller's remote accesses must use the native target's geometry. */
#define _GNU_SOURCE
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "p3s_test.h"
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define MAPPING_SIZE (2 * NATIVE_PAGE_SIZE)

struct child_info {
	uintptr_t address;
	long page_size;
};

static void fill_slices(unsigned char *buffer, unsigned char first)
{
	for (size_t i = 0; i < MAPPING_SIZE; i++)
		buffer[i] = first + i / PROCESS_PAGE_SIZE;
}

static bool check_slices(const unsigned char *buffer, unsigned char first)
{
	for (size_t i = 0; i < MAPPING_SIZE; i++)
		if (buffer[i] != first + i / PROCESS_PAGE_SIZE)
			return false;
	return true;
}

static int run_child(int info_fd, int command_fd)
{
	unsigned char *mapping;
	struct child_info info;
	char command;
	bool ok;

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping, 0x10);
	info.address = (uintptr_t)mapping;
	info.page_size = getpagesize();
	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return KSFT_FAIL;
	ok = command != 'w' || check_slices(mapping, 0x30);
	munmap(mapping, MAPPING_SIZE);
	return ok ? KSFT_PASS : KSFT_FAIL;
}

FIXTURE(remote_access) {
	struct child_info info;
	pid_t child;
	int command_fd;
	bool attached;
};

static bool start_child(FIXTURE_DATA(remote_access) *self)
{
	int info[2] = { -1, -1 }, command[2] = { -1, -1 };
	bool ready = false;

	self->child = -1;
	self->command_fd = -1;
	if (pipe(info) || pipe(command))
		goto out;
	self->child = fork();
	if (self->child < 0)
		goto out;
	if (!self->child) {
		char info_arg[16], command_arg[16];

		close(info[0]);
		close(command[1]);
		snprintf(info_arg, sizeof(info_arg), "%d", info[1]);
		snprintf(command_arg, sizeof(command_arg), "%d", command[0]);
		p3s_execl(false, NULL, "--child", info_arg, command_arg, NULL);
		_exit(KSFT_FAIL);
	}
	close(info[1]);
	info[1] = -1;
	close(command[0]);
	command[0] = -1;
	ready = read_full(info[0], &self->info, sizeof(self->info));
	if (ready) {
		self->command_fd = command[1];
		command[1] = -1;
	}
out:
	for (int i = 0; i < 2; i++) {
		if (info[i] >= 0)
			close(info[i]);
		if (command[i] >= 0)
			close(command[i]);
	}
	if (!ready && self->child > 0) {
		kill(self->child, SIGKILL);
		waitpid(self->child, NULL, 0);
		self->child = -1;
	}
	return ready;
}

FIXTURE_SETUP(remote_access)
{
	signal(SIGPIPE, SIG_IGN);
	ASSERT_TRUE(start_child(self));
	ASSERT_EQ(PROCESS_PAGE_SIZE, getpagesize());
	ASSERT_EQ(NATIVE_PAGE_SIZE, self->info.page_size);
	ASSERT_EQ(0, self->info.address & (NATIVE_PAGE_SIZE - 1));
}

FIXTURE_TEARDOWN(remote_access)
{
	if (self->child > 0) {
		if (self->attached)
			ptrace(PTRACE_DETACH, self->child, NULL, NULL);
		kill(self->child, SIGKILL);
		waitpid(self->child, NULL, 0);
	}
	if (self->command_fd >= 0)
		close(self->command_fd);
}

TEST_F(remote_access, process_vm_readv_preserves_slices_across_native_boundary)
{
	unsigned char buffer[MAPPING_SIZE];
	struct iovec local = { buffer, sizeof(buffer) };
	struct iovec remote = { (void *)self->info.address, sizeof(buffer) };

	ASSERT_EQ(MAPPING_SIZE,
		  process_vm_readv(self->child, &local, 1, &remote, 1, 0));
	EXPECT_TRUE(check_slices(buffer, 0x10));
}

TEST_F(remote_access, proc_mem_preserves_slices_across_native_boundary)
{
	unsigned char buffer[MAPPING_SIZE];
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/mem", self->child);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(MAPPING_SIZE, pread(fd, buffer, sizeof(buffer), self->info.address));
	EXPECT_TRUE(check_slices(buffer, 0x10));
	close(fd);
}

TEST_F(remote_access, pagemap_reports_native_target_pages)
{
	char path[64];
	uint64_t entry;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/pagemap", self->child);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	for (unsigned int i = 0; i < 2; i++) {
		ASSERT_TRUE(pagemap_read_entry(fd,
					       self->info.address + i * NATIVE_PAGE_SIZE,
			NATIVE_PAGE_SIZE, &entry));
		EXPECT_TRUE(entry & PM_PRESENT);
	}
	close(fd);
}

TEST_F(remote_access, native_target_observes_remote_slice_writes)
{
	unsigned char buffer[MAPPING_SIZE];
	struct iovec local = { buffer, sizeof(buffer) };
	struct iovec remote = { (void *)self->info.address, sizeof(buffer) };
	int status;

	fill_slices(buffer, 0x30);
	ASSERT_EQ(MAPPING_SIZE,
		  process_vm_writev(self->child, &local, 1, &remote, 1, 0));
	ASSERT_TRUE(write_full(self->command_fd, "w", 1));
	ASSERT_EQ(self->child, waitpid(self->child, &status, 0));
	self->child = -1;
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(KSFT_PASS, WEXITSTATUS(status));
}

static ssize_t advise(struct __test_metadata *_metadata, pid_t child,
		      uintptr_t address)
{
	struct iovec remote = { (void *)address, PROCESS_PAGE_SIZE };
	int fd = syscall(SYS_pidfd_open, child, 0);
	ssize_t ret;
	int saved;

	ASSERT_GE(fd, 0);
	ret = syscall(SYS_process_madvise, fd, &remote, 1, MADV_WILLNEED, 0);
	saved = errno;
	close(fd);
	errno = saved;
	return ret;
}

TEST_F(remote_access, process_madvise_rejects_unaligned_native_start)
{
	ssize_t ret = advise(_metadata, self->child,
			     self->info.address + PROCESS_PAGE_SIZE);
	int saved = errno;

	if (ret == -1 && saved == EPERM)
		SKIP(return, "process_madvise needs CAP_SYS_NICE");
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, saved);
}

TEST_F(remote_access, process_madvise_accepts_native_aligned_start)
{
	ssize_t ret = advise(_metadata, self->child, self->info.address);

	if (ret == -1 && errno == EPERM)
		SKIP(return, "process_madvise needs CAP_SYS_NICE");
	/* WILLNEED on anonymous memory proves acceptance, not rounding. */
	EXPECT_EQ(PROCESS_PAGE_SIZE, ret);
}

TEST_F(remote_access, ptrace_reads_slice_and_native_boundary)
{
	unsigned long word;
	int status;

	ASSERT_EQ(0, ptrace(PTRACE_ATTACH, self->child, NULL, NULL));
	self->attached = true;
	ASSERT_EQ(self->child, waitpid(self->child, &status, 0));
	ASSERT_TRUE(WIFSTOPPED(status));
	errno = 0;
	word = ptrace(PTRACE_PEEKDATA, self->child,
		      (void *)(self->info.address + PROCESS_PAGE_SIZE), NULL);
	ASSERT_EQ(0, errno);
	EXPECT_EQ(0x1111111111111111UL, word);
	errno = 0;
	word = ptrace(PTRACE_PEEKDATA, self->child,
		      (void *)(self->info.address + NATIVE_PAGE_SIZE), NULL);
	ASSERT_EQ(0, errno);
	EXPECT_EQ(0x1414141414141414UL, word);
	ASSERT_EQ(0, ptrace(PTRACE_DETACH, self->child, NULL, NULL));
	self->attached = false;
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
