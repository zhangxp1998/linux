// SPDX-License-Identifier: GPL-2.0
/*
 * POSIX_FADV_DONTNEED from a 4K compat process works at 4K granularity: a
 * partial-page range leaves the page cached and a full 4K range evicts
 * exactly that page, as observed through cachestat().
 */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

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
		if (!query_cache(fd, TARGET_PAGE * PROCESS_PAGE_SIZE,
				 PROCESS_PAGE_SIZE, &nr_cache))
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
	unsigned char data[PROCESS_PAGE_SIZE];
	__u64 nr_cache = 0;
	int error;
	int fd;

	ppps_require_compat();
	ksft_print_header();
	ksft_set_plan(7);

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	ksft_test_result(fd >= 0, "create the backing file\n");
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));

	memset(data, 0x5a, sizeof(data));
	ksft_test_result(!ftruncate(fd, FILE_PAGES * PROCESS_PAGE_SIZE) &&
			 pwrite(fd, data, sizeof(data),
				TARGET_PAGE * PROCESS_PAGE_SIZE) ==
			 (ssize_t)sizeof(data) && !fsync(fd),
			 "populate and clean the target process page\n");

	ksft_test_result(query_cache(fd, TARGET_PAGE * PROCESS_PAGE_SIZE,
				     PROCESS_PAGE_SIZE, &nr_cache) && nr_cache == 1,
			 "target process page starts resident\n");

	error = posix_fadvise(fd, TARGET_PAGE * PROCESS_PAGE_SIZE + 1,
			      PROCESS_PAGE_SIZE - 2, POSIX_FADV_DONTNEED);
	ksft_test_result(!error, "advise away part of a process page\n");
	ksft_test_result(!error &&
			 query_cache(fd, TARGET_PAGE * PROCESS_PAGE_SIZE,
				     PROCESS_PAGE_SIZE, &nr_cache) && nr_cache == 1,
			 "partial process page remains cached\n");

	error = posix_fadvise(fd, TARGET_PAGE * PROCESS_PAGE_SIZE,
			      PROCESS_PAGE_SIZE, POSIX_FADV_DONTNEED);
	ksft_test_result(!error, "advise away one complete process page\n");
	ksft_test_result(!error && target_page_evicted(fd),
			 "DONTNEED evicts the requested process page\n");

	close(fd);
	unlink(path);
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *mode = ppps_run_mode(argc, argv, NULL);

	if (argc == 2)
		exec_compat(argv[0], PPPS_RUN_FLAG, argv[1], NULL);
	if (argc == 3 && mode && !strcmp(mode, PPPS_RUN_FLAG))
		return run_test(argv[2]);
	return EXIT_FAILURE;
}
