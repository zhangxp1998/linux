// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process maps the VFIO platform fixture's MMIO regions at 4K
 * process-page offsets, including a single-process-page region, and reads
 * the bytes the fixture placed there.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"


#define USER_PAGE_SIZE 4096UL

static int verify_mapping(const unsigned char *mapping)
{
	int fd;
	int ret;

	fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return EXIT_FAILURE;
	ret = write(fd, mapping, 1) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
	close(fd);
	return ret;
}

static int run_test(void)
{
	unsigned char *mapping;
	int status;
	int fd;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = ppps_open_fixture_or_skip("/dev/vfio_platform_mmap_ppps", O_RDWR);
	ksft_test_result(fd >= 0, "open the VFIO platform fixture\n");

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "map one process page\n");
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
			 "mapped VFIO platform page is accessible\n");

	munmap(mapping, PROCESS_PAGE_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
