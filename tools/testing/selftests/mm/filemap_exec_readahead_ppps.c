// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXEC_SIZE (16 * USER_PAGE_SIZE)
#define FILE_SIZE (64 * USER_PAGE_SIZE)

static bool pages_resident(const unsigned char *vec, size_t first, size_t last)
{
	size_t i;

	for (i = first; i < last; i++)
		if (!(vec[i] & 1))
			return false;
	return true;
}

static bool pages_evicted(const unsigned char *vec, size_t first, size_t last)
{
	size_t i;

	for (i = first; i < last; i++)
		if (vec[i] & 1)
			return false;
	return true;
}

static int run_test(const char *path)
{
	unsigned char residency[FILE_SIZE / USER_PAGE_SIZE] = {};
	unsigned char data[FILE_SIZE];
	void *query;
	void *exec;
	ssize_t written;
	int advise_error;
	int nullfd;
	int fd;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	memset(data, 0x5a, sizeof(data));
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		ksft_exit_fail_msg("open test file failed: %s\n", strerror(errno));
	written = pwrite(fd, data, sizeof(data), 0);
	if (written != (ssize_t)sizeof(data) || fdatasync(fd))
		ksft_exit_fail_msg("prepare test file failed: %s\n",
				   strerror(errno));
	advise_error = posix_fadvise(fd, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
	if (advise_error)
		ksft_exit_fail_msg("evict test file failed: %s\n",
				   strerror(advise_error));

	query = mmap(NULL, FILE_SIZE, PROT_NONE, MAP_PRIVATE, fd, 0);
	if (query == MAP_FAILED || mincore(query, FILE_SIZE, residency))
		ksft_exit_fail_msg("query mapping failed: %s\n", strerror(errno));
	ksft_test_result(pages_evicted(residency, 0, sizeof(residency)),
			 "evict the complete test file from page cache\n");

	exec = mmap(NULL, EXEC_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
	if (exec == MAP_FAILED || madvise(exec, EXEC_SIZE, MADV_RANDOM))
		ksft_exit_fail_msg("executable mapping failed: %s\n",
				   strerror(errno));
	nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	ksft_test_result(nullfd >= 0 && write(nullfd, exec, 1) == 1,
			 "fault the executable VMA\n");

	memset(residency, 0, sizeof(residency));
	if (mincore(query, FILE_SIZE, residency))
		ksft_exit_fail_msg("mincore after fault failed: %s\n",
				   strerror(errno));
	ksft_test_result(pages_resident(residency, 0,
					EXEC_SIZE / USER_PAGE_SIZE),
			 "populate the executable VMA\n");
	ksft_test_result(pages_evicted(residency,
					EXEC_SIZE / USER_PAGE_SIZE,
					sizeof(residency)),
			 "do not read ahead beyond the executable VMA\n");

	if (nullfd >= 0)
		close(nullfd);
	munmap(exec, EXEC_SIZE);
	munmap(query, FILE_SIZE);
	close(fd);
	unlink(path);
	ksft_finished();
}

static int exec_compat(const char *path)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "filemap_exec_readahead_ppps", "--run",
	      path, NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	char path[PATH_MAX];

	if (argc == 1) {
		if (snprintf(path, sizeof(path),
			     "filemap_exec_readahead_ppps.%ld", (long)getpid()) >=
		    (int)sizeof(path))
			return EXIT_FAILURE;
		return exec_compat(path);
	}
	if (argc == 2)
		return exec_compat(argv[1]);
	if (argc == 3 && !strcmp(argv[1], "--run"))
		return run_test(argv[2]);
	return EXIT_FAILURE;
}
