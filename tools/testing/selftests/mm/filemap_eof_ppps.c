// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define FILE_SIZE 5000UL

static int read_status(const unsigned char *address)
{
	int status;
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (!child)
		_exit(!!*address);
	if (waitpid(child, &status, 0) != child)
		return -1;
	return status;
}

static bool read_succeeds(const unsigned char *address)
{
	int status = read_status(address);

	return status >= 0 && WIFEXITED(status);
}

static bool read_gets_sigbus(const unsigned char *address)
{
	int status = read_status(address);

	return status >= 0 && WIFSIGNALED(status) &&
	       WTERMSIG(status) == SIGBUS;
}

static int run_compat_test(const char *mount_dir)
{
	unsigned char marker = 0x5a;
	unsigned char *mapping;
	unsigned char *offset_mapping;
	char path[256];
	bool initialized;
	int fd;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	if (snprintf(path, sizeof(path), "%s/filemap-eof.bin", mount_dir) >=
	    (int)sizeof(path))
		ksft_exit_fail_msg("test path is too long\n");
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
	initialized = fd >= 0 && !ftruncate(fd, FILE_SIZE) &&
		pwrite(fd, &marker, 1, 0) == 1 &&
		pwrite(fd, &marker, 1, USER_PAGE_SIZE) == 1 && !fsync(fd);
	ksft_test_result(initialized, "create a 5000-byte regular file\n");
	if (!initialized)
		ksft_exit_fail_msg("file initialization failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, 3 * USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map three 4K pages at file offset zero\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("first mmap failed: %s\n", strerror(errno));
	ksft_test_result(read_succeeds(mapping),
			 "first file page is readable\n");
	ksft_test_result(read_succeeds(mapping + USER_PAGE_SIZE),
			 "partial final file page is readable\n");
	ksft_test_result(read_gets_sigbus(mapping + 2 * USER_PAGE_SIZE),
			 "first complete 4K page beyond EOF raises SIGBUS\n");

	offset_mapping = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ, MAP_SHARED,
			      fd, USER_PAGE_SIZE);
	ksft_test_result(offset_mapping != MAP_FAILED,
			 "map two 4K pages at a 4K file offset\n");
	if (offset_mapping == MAP_FAILED)
		ksft_exit_fail_msg("offset mmap failed: %s\n", strerror(errno));
	ksft_test_result(read_succeeds(offset_mapping),
			 "offset mapping starts in the file\n");
	ksft_test_result(read_gets_sigbus(offset_mapping + USER_PAGE_SIZE),
			 "offset mapping preserves 4K EOF SIGBUS semantics\n");

	munmap(offset_mapping, 2 * USER_PAGE_SIZE);
	munmap(mapping, 3 * USER_PAGE_SIZE);
	close(fd);
	unlink(path);
	ksft_finished();
}

static int exec_compat_test(const char *mount_dir)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	execl("/proc/self/exe", "filemap_eof_ppps", "--compat", mount_dir,
	      NULL);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	if (argc == 2)
		return exec_compat_test(argv[1]);
	if (argc == 3 && !strcmp(argv[1], "--compat"))
		return run_compat_test(argv[2]);
	return EXIT_FAILURE;
}
