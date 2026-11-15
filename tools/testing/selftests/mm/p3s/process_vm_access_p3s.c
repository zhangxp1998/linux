// SPDX-License-Identifier: GPL-2.0
/*
 * process_vm_readv()/process_vm_writev() from a native (16K) caller transfer
 * two 4K pages of a 4K compat child, both anonymous and file-backed at a
 * 4K-only file offset, with the contents intact in each direction.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

#define TRANSFER_SIZE	(2 * PROCESS_PAGE_SIZE)
#define MAPPING_SIZE	(16 * PROCESS_PAGE_SIZE)
#define FILE_MAPPING_OFFSET	(3 * PROCESS_PAGE_SIZE)
#define FILE_SIZE	(16 * PROCESS_PAGE_SIZE)

struct child_info {
	uintptr_t anon_address;
	uintptr_t file_address;
	long page_size;
};

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

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

static int run_child(int info_fd, int command_fd)
{
	unsigned char file_contents[TRANSFER_SIZE];
	unsigned char *file_mapping;
	unsigned char *mapping;
	unsigned char *target;
	struct child_info info;
	char command;
	bool passed;
	int memfd;

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("child mmap");
		return EXIT_FAILURE;
	}
	target = (unsigned char *)align_up((uintptr_t)mapping, NATIVE_PAGE_SIZE) +
		 3 * PROCESS_PAGE_SIZE;
	if (target + TRANSFER_SIZE > mapping + MAPPING_SIZE)
		return EXIT_FAILURE;
	memset(target, 0x11, PROCESS_PAGE_SIZE);
	memset(target + PROCESS_PAGE_SIZE, 0x22, PROCESS_PAGE_SIZE);

	memfd = memfd_create("process-vm-access-p3s", 0);
	if (memfd < 0 || ftruncate(memfd, FILE_SIZE)) {
		perror("child memfd setup");
		return EXIT_FAILURE;
	}
	memset(file_contents, 0x55, PROCESS_PAGE_SIZE);
	memset(file_contents + PROCESS_PAGE_SIZE, 0x66, PROCESS_PAGE_SIZE);
	if (!pwrite_full(memfd, file_contents, sizeof(file_contents),
			 FILE_MAPPING_OFFSET)) {
		perror("child pwrite");
		return EXIT_FAILURE;
	}
	file_mapping = mmap(NULL, TRANSFER_SIZE, PROT_READ | PROT_WRITE,
			    MAP_SHARED, memfd, FILE_MAPPING_OFFSET);
	if (file_mapping == MAP_FAILED) {
		perror("child file mmap");
		return EXIT_FAILURE;
	}

	info.anon_address = (uintptr_t)target;
	info.file_address = (uintptr_t)file_mapping;
	info.page_size = sysconf(_SC_PAGESIZE);
	if (!write_full(info_fd, &info, sizeof(info)))
		return EXIT_FAILURE;
	if (!read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;

	passed = (!(command & 1) ||
		  (buffer_is_value(target, PROCESS_PAGE_SIZE, 0x33) &&
		   buffer_is_value(target + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
				   0x44))) &&
		 (!(command & 2) ||
		  (buffer_is_value(file_mapping, PROCESS_PAGE_SIZE, 0x77) &&
		   buffer_is_value(file_mapping + PROCESS_PAGE_SIZE,
				   PROCESS_PAGE_SIZE, 0x88)));
	munmap(file_mapping, TRANSFER_SIZE);
	close(memfd);
	munmap(mapping, MAPPING_SIZE);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

FIXTURE(process_vm_access) {
	struct child_info info;
	int command_fd;
	pid_t child;
	char command;
};

static bool start_child(FIXTURE_DATA(process_vm_access) *self)
{
	int info_pipe[2] = { -1, -1 };
	int command_pipe[2] = { -1, -1 };
	int status;
	bool ready = false;

	self->child = -1;
	self->command_fd = -1;
	self->command = 0;
	if (pipe(info_pipe) || pipe(command_pipe))
		goto out;
	self->child = fork();
	if (self->child < 0)
		goto out;
	if (!self->child) {
		char command_fd[16];
		char info_fd[16];

		close(info_pipe[0]);
		close(command_pipe[1]);
		snprintf(info_fd, sizeof(info_fd), "%d", info_pipe[1]);
		snprintf(command_fd, sizeof(command_fd), "%d", command_pipe[0]);
		p3s_execl(true, NULL, "--child", info_fd, command_fd, NULL);
		_exit(127);
	}
	close(info_pipe[1]);
	info_pipe[1] = -1;
	close(command_pipe[0]);
	command_pipe[0] = -1;
	ready = read_full(info_pipe[0], &self->info, sizeof(self->info));
	if (ready && (self->info.page_size != PROCESS_PAGE_SIZE ||
		      (self->info.anon_address & (NATIVE_PAGE_SIZE - 1)) !=
		      3 * PROCESS_PAGE_SIZE))
		ready = false;
	if (ready) {
		self->command_fd = command_pipe[1];
		command_pipe[1] = -1;
	}
out:
	if (info_pipe[0] >= 0)
		close(info_pipe[0]);
	if (info_pipe[1] >= 0)
		close(info_pipe[1]);
	if (command_pipe[0] >= 0)
		close(command_pipe[0]);
	if (command_pipe[1] >= 0)
		close(command_pipe[1]);
	if (!ready && self->child > 0) {
		waitpid(self->child, &status, 0);
		self->child = -1;
	}
	return ready;
}

FIXTURE_SETUP(process_vm_access)
{
	ASSERT_TRUE(start_child(self));
	signal(SIGPIPE, SIG_IGN);
}

FIXTURE_TEARDOWN(process_vm_access)
{
	int status = 0;

	if (self->command_fd >= 0) {
		EXPECT_TRUE(write_full(self->command_fd, &self->command,
				       sizeof(self->command)));
		close(self->command_fd);
	}
	EXPECT_EQ(self->child, waitpid(self->child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
		EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
}

TEST_F(process_vm_access, read_two_anonymous_4k_pages)
{
	unsigned char buffer[TRANSFER_SIZE] = {};
	struct iovec local = { .iov_base = buffer, .iov_len = sizeof(buffer) };
	struct iovec remote = { .iov_base = (void *)self->info.anon_address,
				.iov_len = sizeof(buffer) };

	EXPECT_EQ(TRANSFER_SIZE,
		  process_vm_readv(self->child, &local, 1, &remote, 1, 0));
	EXPECT_TRUE(buffer_is_value(buffer, PROCESS_PAGE_SIZE, 0x11));
	EXPECT_TRUE(buffer_is_value(buffer + PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE, 0x22));
}

TEST_F(process_vm_access, write_two_anonymous_4k_pages)
{
	unsigned char buffer[TRANSFER_SIZE];
	struct iovec local = { .iov_base = buffer, .iov_len = sizeof(buffer) };
	struct iovec remote = { .iov_base = (void *)self->info.anon_address,
				.iov_len = sizeof(buffer) };

	memset(buffer, 0x33, PROCESS_PAGE_SIZE);
	memset(buffer + PROCESS_PAGE_SIZE, 0x44, PROCESS_PAGE_SIZE);
	self->command = 1;
	EXPECT_EQ(TRANSFER_SIZE,
		  process_vm_writev(self->child, &local, 1, &remote, 1, 0));
}

TEST_F(process_vm_access, read_file_backed_4k_offset_pages)
{
	unsigned char buffer[TRANSFER_SIZE] = {};
	struct iovec local = { .iov_base = buffer, .iov_len = sizeof(buffer) };
	struct iovec remote = { .iov_base = (void *)self->info.file_address,
				.iov_len = sizeof(buffer) };

	EXPECT_EQ(TRANSFER_SIZE,
		  process_vm_readv(self->child, &local, 1, &remote, 1, 0));
	EXPECT_TRUE(buffer_is_value(buffer, PROCESS_PAGE_SIZE, 0x55));
	EXPECT_TRUE(buffer_is_value(buffer + PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE, 0x66));
}

TEST_F(process_vm_access, write_file_backed_4k_offset_pages)
{
	unsigned char buffer[TRANSFER_SIZE];
	struct iovec local = { .iov_base = buffer, .iov_len = sizeof(buffer) };
	struct iovec remote = { .iov_base = (void *)self->info.file_address,
				.iov_len = sizeof(buffer) };

	memset(buffer, 0x77, PROCESS_PAGE_SIZE);
	memset(buffer + PROCESS_PAGE_SIZE, 0x88, PROCESS_PAGE_SIZE);
	self->command = 2;
	EXPECT_EQ(TRANSFER_SIZE,
		  process_vm_writev(self->child, &local, 1, &remote, 1, 0));
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
