// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/landlock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"
#include "wrappers.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct landlock_ruleset_attr attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE,
	};
	unsigned char *extended;
	int saved_errno;
	int abi;
	int fd;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	ksft_test_result(abi > 0, "Landlock ABI is available\n");
	fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ksft_test_result(fd >= 0, "create a baseline Landlock ruleset\n");
	if (fd >= 0)
		close(fd);

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended,
			 "allocate a zero-extended 4097-byte attribute\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &attr, sizeof(attr));
	errno = 0;
	fd = landlock_create_ruleset((void *)extended, EXTENDED_SIZE, 0);
	saved_errno = errno;
	ksft_test_result(fd == -1 && saved_errno == E2BIG,
			 "bound attribute size to the process page\n");
	if (fd >= 0)
		close(fd);
	free(extended);
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
	execl("/proc/self/exe", "landlock_ppps_test", "--run", NULL);
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
