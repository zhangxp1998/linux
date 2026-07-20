// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/mman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define FILE_PAGES 4UL
#define TARGET_PAGE 1UL
#define POLL_ATTEMPTS 20

static bool query_cache(int fd, __u64 offset, __u64 length, __u64 *nr_cache)
{
	struct cachestat_range range = {
		.off = offset,
		.len = length,
	};
	struct cachestat stat;

	memset(&stat, 0, sizeof(stat));
	if (syscall(__NR_cachestat, fd, &range, &stat, 0)) {
		ksft_print_msg("cachestat failed: %s\n", strerror(errno));
		return false;
	}
	*nr_cache = stat.nr_cache;
	return true;
}

static bool target_page_evicted(int fd)
{
	unsigned int attempt;
	__u64 nr_cache;

	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!query_cache(fd, TARGET_PAGE * USER_PAGE_SIZE,
				 USER_PAGE_SIZE, &nr_cache))
			return false;
		if (!nr_cache) {
			ksft_print_msg("target cache pages after DONTNEED: 0\n");
			return true;
		}
		usleep(10000);
	}
	ksft_print_msg("target page remained resident\n");
	return false;
}

static int run_test(const char *path)
{
	unsigned char data[USER_PAGE_SIZE];
	__u64 nr_cache = 0;
	int error;
	int fd;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	ksft_test_result(fd >= 0, "create the backing file\n");
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));

	memset(data, 0x5a, sizeof(data));
	ksft_test_result(!ftruncate(fd, FILE_PAGES * USER_PAGE_SIZE) &&
			 pwrite(fd, data, sizeof(data),
				TARGET_PAGE * USER_PAGE_SIZE) ==
			 (ssize_t)sizeof(data) && !fsync(fd),
			 "populate and clean the target process page\n");

	ksft_test_result(query_cache(fd, TARGET_PAGE * USER_PAGE_SIZE,
				     USER_PAGE_SIZE, &nr_cache) && nr_cache == 1,
			 "target process page starts resident\n");

	error = posix_fadvise(fd, TARGET_PAGE * USER_PAGE_SIZE + 1,
			      USER_PAGE_SIZE - 2, POSIX_FADV_DONTNEED);
	ksft_test_result(!error, "advise away part of a process page\n");
	ksft_test_result(!error &&
			 query_cache(fd, TARGET_PAGE * USER_PAGE_SIZE,
				     USER_PAGE_SIZE, &nr_cache) && nr_cache == 1,
			 "partial process page remains cached\n");

	error = posix_fadvise(fd, TARGET_PAGE * USER_PAGE_SIZE,
			      USER_PAGE_SIZE, POSIX_FADV_DONTNEED);
	ksft_test_result(!error, "advise away one complete process page\n");
	ksft_test_result(!error && target_page_evicted(fd),
			 "DONTNEED evicts the requested process page\n");

	close(fd);
	unlink(path);
	ksft_finished();
}

static int exec_compat(const char *path)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("could not enable 4K compatibility mode\n");
	execl("/proc/self/exe", "fadvise_dontneed_ppps", "--compat", path,
	      NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 2)
		return exec_compat(argv[1]);
	if (argc == 3 && !strcmp(argv[1], "--compat"))
		return run_test(argv[2]);
	return EXIT_FAILURE;
}
