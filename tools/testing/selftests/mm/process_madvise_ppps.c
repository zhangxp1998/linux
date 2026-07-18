// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE	0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define LARGE_PAGE_SIZE	16384UL
#define MAPPING_SIZE	(16 * USER_PAGE_SIZE)

struct child_info {
	uintptr_t address;
	long page_size;
};

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool write_full(int fd, const void *buffer, size_t length)
{
	const unsigned char *pos = buffer;

	while (length) {
		ssize_t written = write(fd, pos, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		pos += written;
		length -= written;
	}
	return true;
}

static bool read_full(int fd, void *buffer, size_t length)
{
	unsigned char *pos = buffer;

	while (length) {
		ssize_t bytes = read(fd, pos, length);

		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (!bytes)
			return false;
		pos += bytes;
		length -= bytes;
	}
	return true;
}

static int current_personality(void)
{
	return personality(0xffffffffUL);
}

static int bootstrap_parent(void)
{
	int persona = current_personality();

	if (persona < 0) {
		perror("personality(get)");
		return EXIT_FAILURE;
	}
	if (personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0) {
		perror("personality(clear 4K flag)");
		return EXIT_FAILURE;
	}
	execl("/proc/self/exe", "process_madvise_ppps", "--parent", NULL);
	perror("exec parent");
	return EXIT_FAILURE;
}

static int exec_compat_child(int info_fd, int done_fd)
{
	char done_fd_arg[32];
	char info_fd_arg[32];
	int persona = current_personality();

	if (persona < 0) {
		perror("personality(get child)");
		return EXIT_FAILURE;
	}
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0) {
		perror("personality(set 4K flag)");
		return EXIT_FAILURE;
	}
	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(done_fd_arg, sizeof(done_fd_arg), "%d", done_fd);
	execl("/proc/self/exe", "process_madvise_ppps", "--child",
	      info_fd_arg, done_fd_arg, NULL);
	perror("exec child");
	return EXIT_FAILURE;
}

static int run_child(int info_fd, int done_fd)
{
	unsigned char *large_page_aligned;
	unsigned char *mapping;
	struct child_info info;
	char done;

	info.page_size = sysconf(_SC_PAGESIZE);
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("child mmap");
		return EXIT_FAILURE;
	}
	large_page_aligned = (unsigned char *)align_up((uintptr_t)mapping,
							LARGE_PAGE_SIZE);
	if (large_page_aligned + 2 * USER_PAGE_SIZE > mapping + MAPPING_SIZE)
		return EXIT_FAILURE;
	info.address = (uintptr_t)(large_page_aligned + USER_PAGE_SIZE);

	if (!write_full(info_fd, &info, sizeof(info)))
		return EXIT_FAILURE;
	if (!read_full(done_fd, &done, sizeof(done)))
		return EXIT_FAILURE;
	munmap(mapping, MAPPING_SIZE);
	return EXIT_SUCCESS;
}

static int run_parent(void)
{
	struct child_info info;
	struct iovec iov;
	ssize_t advised;
	int done_pipe[2];
	int info_pipe[2];
	int child_status;
	int pidfd;
	long parent_page_size;
	char done = 1;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(3);
	parent_page_size = sysconf(_SC_PAGESIZE);

	if (pipe(info_pipe) || pipe(done_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(info_pipe[0]);
		close(done_pipe[1]);
		_exit(exec_compat_child(info_pipe[1], done_pipe[0]));
	}

	close(info_pipe[1]);
	close(done_pipe[0]);
	if (!read_full(info_pipe[0], &info, sizeof(info)))
		ksft_exit_fail_msg("failed to read child mapping information\n");
	close(info_pipe[0]);
	ksft_print_msg("caller page size=%ld, target page size=%ld, address=%#lx\n",
		       parent_page_size, info.page_size,
		       (unsigned long)info.address);
	ksft_test_result(info.page_size == USER_PAGE_SIZE &&
			 !(info.address & (USER_PAGE_SIZE - 1)) &&
			 (info.address & (LARGE_PAGE_SIZE - 1)),
			 "target exposes a 4K-only-aligned range\n");

	pidfd = syscall(__NR_pidfd_open, child, 0);
	if (pidfd < 0)
		ksft_exit_fail_msg("pidfd_open failed: %s\n", strerror(errno));
	iov.iov_base = (void *)info.address;
	iov.iov_len = USER_PAGE_SIZE;
	errno = 0;
	advised = syscall(__NR_process_madvise, pidfd, &iov, 1,
			  MADV_WILLNEED, 0);
	if (advised != USER_PAGE_SIZE)
		ksft_print_msg("process_madvise returned %zd: %s\n", advised,
			       advised < 0 ? strerror(errno) : "short operation");
	ksft_test_result(advised == USER_PAGE_SIZE,
			 "process_madvise uses the target mm's 4K granularity\n");
	close(pidfd);

	if (!write_full(done_pipe[1], &done, sizeof(done)))
		ksft_print_msg("failed to release child: %s\n", strerror(errno));
	close(done_pipe[1]);
	if (waitpid(child, &child_status, 0) < 0)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(child_status) &&
			 WEXITSTATUS(child_status) == EXIT_SUCCESS,
			 "4K target child exits cleanly\n");

	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return bootstrap_parent();
	if (!strcmp(argv[1], "--parent"))
		return run_parent();
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return EXIT_FAILURE;
}
