// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/lsm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <unistd.h>

#include "common.h"
#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct lsm_ctx baseline = {
		.id = LSM_ID_UNDEF,
		.len = sizeof(baseline),
	};
	struct lsm_ctx *extended;
	int saved_errno;
	int ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	errno = 0;
	ret = lsm_set_self_attr(LSM_ATTR_CURRENT, &baseline,
				sizeof(baseline), 0);
	ksft_test_result(ret == -1 && errno == EOPNOTSUPP,
			 "reach the generic LSM hook with a baseline context\n");

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended,
			 "allocate a zero-extended LSM context\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &baseline, sizeof(baseline));
	errno = 0;
	ret = lsm_set_self_attr(LSM_ATTR_CURRENT, extended, EXTENDED_SIZE, 0);
	saved_errno = errno;
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound context size to the process page\n");
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
	execl("/proc/self/exe", "lsm_attr_size_ppps", "--run", NULL);
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
