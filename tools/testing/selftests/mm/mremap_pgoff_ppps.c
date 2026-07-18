// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process's shared mapping of the fixture file at a 4K file
 * offset keeps that process-page offset when mremap grows and moves it.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_ppps.h"

#define DEVICE_PATH "/dev/mremap_pgoff_ppps"

static int run_test(void)
{
	void *mapping;
	void *moved;
	void *guard;
	void *reserve;
	int fd;

	ksft_print_header();
	ksft_set_plan(2);

	fd = ppps_open_fixture_or_skip(DEVICE_PATH, O_RDONLY);
	reserve = mmap(NULL, 2 * PROCESS_PAGE_SIZE, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reserve == MAP_FAILED)
		ksft_exit_fail_msg("reserve mmap failed: %s\n", strerror(errno));
	guard = reserve + PROCESS_PAGE_SIZE;
	if (munmap(reserve, PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("reserve munmap failed: %s\n", strerror(errno));
	mapping = mmap(reserve, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, PROCESS_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the test file at a 4K offset\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	errno = 0;
	moved = mremap(mapping, PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
		       MREMAP_MAYMOVE);
	ksft_test_result(moved != MAP_FAILED,
			 "preserve the process-page file offset during mremap\n");
	if (moved == MAP_FAILED)
		ksft_print_msg("mremap failed: %s\n", strerror(errno));
	else
		munmap(moved, 2 * PROCESS_PAGE_SIZE);
	if (moved == MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	munmap(guard, PROCESS_PAGE_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
