// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE	0x10000000
#endif

#define COMPAT_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE	16384UL
#define MAPPING_SIZE		(2 * NATIVE_PAGE_SIZE)
#define PAGEMAP_PRESENT		UINT64_C(0x8000000000000000)
#define ADDRESS_TAG		(UINT64_C(0xb4) << 56)

struct child_info {
	uintptr_t address;
	long page_size;
};

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

static int set_page_size_and_exec(bool compat, const char *mode,
				  int info_fd, int command_fd)
{
	char command_fd_arg[16];
	char info_fd_arg[16];
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		return EXIT_FAILURE;
	if (compat)
		persona |= ADDR_4KB_COMPAT_PAGE_SIZE;
	else
		persona &= ~ADDR_4KB_COMPAT_PAGE_SIZE;
	if (personality(persona) < 0)
		return EXIT_FAILURE;
	if (!strcmp(mode, "--parent")) {
		execl("/proc/self/exe", "mixed_mm_access_ppps", mode, NULL);
	} else {
		snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
		snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
		execl("/proc/self/exe", "mixed_mm_access_ppps", mode,
		      info_fd_arg, command_fd_arg, NULL);
	}
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

static int run_parent(void)
{
	unsigned char read_buffer[MAPPING_SIZE];
	unsigned char write_buffer[MAPPING_SIZE];
	uint64_t pagemap[2] = {};
	struct child_info info;
	struct iovec local;
	struct iovec remote;
	ssize_t result;
	unsigned long word0 = 0;
	unsigned long word1 = 0;
	int command_pipe[2];
	int info_pipe[2];
	int child_status;
	int pidfd;
	char command = 1;
	pid_t child;
	bool attached = false;

	ksft_print_header();
	ksft_set_plan(14);
	if (pipe(info_pipe) || pipe(command_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(info_pipe[0]);
		close(command_pipe[1]);
		_exit(set_page_size_and_exec(false, "--child", info_pipe[1],
					     command_pipe[0]));
	}
	close(info_pipe[1]);
	close(command_pipe[0]);
	if (!read_full(info_pipe[0], &info, sizeof(info)))
		ksft_exit_fail_msg("failed to read child information\n");
	close(info_pipe[0]);
	ksft_print_msg("caller page size=%ld, target page size=%ld, address=%#lx\n",
		       sysconf(_SC_PAGESIZE), info.page_size,
		       (unsigned long)info.address);
	ksft_test_result(sysconf(_SC_PAGESIZE) == COMPAT_PAGE_SIZE &&
			 info.page_size == NATIVE_PAGE_SIZE &&
			 !(info.address & (NATIVE_PAGE_SIZE - 1)),
			 "4K caller targets a native-16K process\n");

	local.iov_base = read_buffer;
	local.iov_len = sizeof(read_buffer);
	remote.iov_base = (void *)info.address;
	remote.iov_len = sizeof(read_buffer);
	result = process_vm_readv(child, &local, 1, &remote, 1, 0);
	ksft_test_result(result == MAPPING_SIZE,
			 "process_vm_readv crosses a native-page boundary\n");
	ksft_test_result(buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
			 buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
					 NATIVE_PAGE_SIZE, 0x22),
			 "process_vm_readv preserves both native pages\n");
	memset(read_buffer, 0, sizeof(read_buffer));
	remote.iov_base = (void *)(info.address | ADDRESS_TAG);
	result = process_vm_readv(child, &local, 1, &remote, 1, 0);
	ksft_test_result(result == MAPPING_SIZE &&
			 buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
			 buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
					 NATIVE_PAGE_SIZE, 0x22),
			 "process_vm_readv accepts a tagged native target address\n");
	remote.iov_base = (void *)info.address;

	memset(read_buffer, 0, sizeof(read_buffer));
	result = read_process_mem(child, info.address, read_buffer,
				  sizeof(read_buffer));
	ksft_test_result(result == MAPPING_SIZE,
			 "/proc/pid/mem crosses a native-page boundary\n");
	ksft_test_result(buffer_is_value(read_buffer, NATIVE_PAGE_SIZE, 0x11) &&
			 buffer_is_value(read_buffer + NATIVE_PAGE_SIZE,
					 NATIVE_PAGE_SIZE, 0x22),
			 "/proc/pid/mem uses the target geometry\n");

	result = read_process_pagemap(child, info.address, pagemap);
	ksft_test_result(result == sizeof(pagemap),
			 "/proc/pid/pagemap accepts a native target offset\n");
	ksft_test_result((pagemap[0] & PAGEMAP_PRESENT) &&
			 (pagemap[1] & PAGEMAP_PRESENT),
			 "/proc/pid/pagemap reports both native pages\n");

	memset(write_buffer, 0x33, NATIVE_PAGE_SIZE);
	memset(write_buffer + NATIVE_PAGE_SIZE, 0x44, NATIVE_PAGE_SIZE);
	local.iov_base = write_buffer;
	local.iov_len = sizeof(write_buffer);
	result = process_vm_writev(child, &local, 1, &remote, 1, 0);
	ksft_test_result(result == MAPPING_SIZE,
			 "process_vm_writev crosses a native-page boundary\n");

	pidfd = syscall(__NR_pidfd_open, child, 0);
	if (pidfd < 0)
		ksft_exit_fail_msg("pidfd_open failed: %s\n", strerror(errno));
	remote.iov_base = (void *)(info.address + COMPAT_PAGE_SIZE);
	remote.iov_len = COMPAT_PAGE_SIZE;
	errno = 0;
	result = syscall(__NR_process_madvise, pidfd, &remote, 1,
			 MADV_WILLNEED, 0);
	ksft_test_result(result == -1 && errno == EINVAL,
			 "process_madvise rejects a target-native unaligned start\n");
	remote.iov_base = (void *)info.address;
	errno = 0;
	result = syscall(__NR_process_madvise, pidfd, &remote, 1,
			 MADV_WILLNEED, 0);
	ksft_test_result(result == COMPAT_PAGE_SIZE,
			 "process_madvise rounds length using target geometry\n");
	close(pidfd);

	if (!ptrace(PTRACE_ATTACH, child, NULL, NULL) &&
	    waitpid(child, &child_status, 0) == child &&
	    WIFSTOPPED(child_status))
		attached = true;
	ksft_test_result(attached, "ptrace attaches across page-size modes\n");
	if (attached) {
		errno = 0;
		word0 = ptrace(PTRACE_PEEKDATA, child,
			       (void *)(info.address + COMPAT_PAGE_SIZE), NULL);
		if (errno)
			attached = false;
		errno = 0;
		word1 = ptrace(PTRACE_PEEKDATA, child,
			       (void *)(info.address + NATIVE_PAGE_SIZE), NULL);
		if (errno)
			attached = false;
		ptrace(PTRACE_DETACH, child, NULL, NULL);
	}
	ksft_test_result(attached &&
			 word0 == 0x3333333333333333UL &&
			 word1 == 0x4444444444444444UL,
			 "ptrace reads a 4K slice and native-page boundary\n");

	if (!write_full(command_pipe[1], &command, sizeof(command)))
		ksft_print_msg("failed to release child: %s\n", strerror(errno));
	close(command_pipe[1]);
	if (waitpid(child, &child_status, 0) < 0)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(child_status) &&
			 WEXITSTATUS(child_status) == EXIT_SUCCESS,
			 "native target observes all remote writes\n");
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return set_page_size_and_exec(true, "--parent", -1, -1);
	if (argc == 2 && !strcmp(argv[1], "--parent"))
		return run_parent();
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return EXIT_FAILURE;
}
