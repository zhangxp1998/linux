// SPDX-License-Identifier: GPL-2.0
/*
 * A 16K TTM buffer mapped by a 4K compat process is prefaulted within its VMA
 * and a forked child reads the fixture's marker from the mapping.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define MAPPING_SIZE (4 * PROCESS_PAGE_SIZE)

static int verify_mapping(const unsigned char *mapping)
{
	static const unsigned char marker[] = "TTM-PPPS";

	return memcmp(mapping, marker, sizeof(marker) - 1) ?
		EXIT_FAILURE : EXIT_SUCCESS;
}

static int run_test(void)
{
	unsigned char *mapping;
	int status;
	int fd;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(3);

	fd = ppps_open_fixture_or_skip("/dev/ttm_vm_fault_ppps", O_RDWR);
	ksft_test_result(fd >= 0, "open the TTM fault fixture\n");

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "map one 16K logical buffer\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!pid)
		_exit(verify_mapping(mapping));
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			 "TTM prefault stays within the VMA\n");

	munmap(mapping, MAPPING_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
