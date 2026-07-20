// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__

#include <errno.h>
#include <fcntl.h>
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

static bool query_pages(int fd, __u64 offset, __u64 length,
			__u64 expected)
{
	struct cachestat_range range = {
		.off = offset,
		.len = length,
	};
	struct cachestat stat;

	memset(&stat, 0, sizeof(stat));
	if (syscall(__NR_cachestat, fd, &range, &stat, 0)) {
		ksft_print_msg("cachestat(%llu, %llu) failed: %s\n",
			       offset, length, strerror(errno));
		return false;
	}

	ksft_print_msg("range=%llu+%llu cache=%llu dirty=%llu\n",
		       offset, length, stat.nr_cache, stat.nr_dirty);
	return stat.nr_cache == expected;
}

static int run_test(void)
{
	char data[USER_PAGE_SIZE * FILE_PAGES];
	int fd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = memfd_create("cachestat-ppps", MFD_CLOEXEC);
	ksft_test_result(fd >= 0, "create a cache-backed file\n");
	if (fd < 0)
		ksft_exit_fail_msg("memfd_create failed: %s\n", strerror(errno));

	memset(data, 0x5a, sizeof(data));
	ksft_test_result(write(fd, data, sizeof(data)) == sizeof(data),
			 "cache four process pages\n");
	ksft_test_result(query_pages(fd, 0, sizeof(data), FILE_PAGES),
			 "full range is reported in process pages\n");
	ksft_test_result(query_pages(fd, USER_PAGE_SIZE, USER_PAGE_SIZE, 1),
			 "single subpage range is not overcounted\n");
	ksft_test_result(query_pages(fd, USER_PAGE_SIZE,
				     2 * USER_PAGE_SIZE, 2),
			 "middle range is reported in process pages\n");

	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("could not enable 4K compatibility mode\n");
	execl("/proc/self/exe", "cachestat_ppps", "--compat", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return run_test();
	return EXIT_FAILURE;
}
