// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"
#include "folio_within_range_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static int run_test(void)
{
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/folio_within_range_ppps", O_RDONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the folio range fixture\n");
	if (fd < 0)
		ksft_exit_fail_msg("open fixture failed: %s\n", strerror(errno));

	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_PPPS_CHECK_ALIGNED) == 0,
			 "recognize a completely covered native folio\n");
	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_PPPS_CHECK_SUBPAGE) == 0,
			 "reject a folio only partially covered at VMA start\n");

	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "folio_within_range_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
