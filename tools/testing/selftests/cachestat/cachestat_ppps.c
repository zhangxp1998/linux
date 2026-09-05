// SPDX-License-Identifier: GPL-2.0
/*
 * cachestat() on a memfd written by a 4K compat process reports nr_cache in
 * 4K process pages for the full file, a single page and a middle range.
 */
#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__

#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

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
	char data[PROCESS_PAGE_SIZE * FILE_PAGES];
	int fd;

	ksft_print_header();
	ksft_set_plan(5);

	fd = memfd_create("cachestat-ppps", MFD_CLOEXEC);
	ksft_test_result(fd >= 0, "create a cache-backed file\n");
	if (fd < 0)
		ksft_exit_fail_msg("memfd_create failed: %s\n", strerror(errno));

	memset(data, 0x5a, sizeof(data));
	ksft_test_result(write(fd, data, sizeof(data)) == sizeof(data),
			 "cache four process pages\n");
	ksft_test_result(query_pages(fd, 0, sizeof(data), FILE_PAGES),
			 "full range is reported in process pages\n");
	ksft_test_result(query_pages(fd, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE, 1),
			 "single subpage range is not overcounted\n");
	ksft_test_result(query_pages(fd, PROCESS_PAGE_SIZE,
				     2 * PROCESS_PAGE_SIZE, 2),
			 "middle range is reported in process pages\n");

	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
