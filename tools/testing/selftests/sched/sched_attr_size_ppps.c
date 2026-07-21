// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/sched/types.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_sched_setattr
#define __NR_sched_setattr 274
#endif

#ifndef __NR_sched_getattr
#define __NR_sched_getattr 275
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct sched_attr baseline = {};
	struct sched_attr *extended;
	int saved_errno;
	int ret;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ret = syscall(__NR_sched_getattr, 0, &baseline, sizeof(baseline), 0);
	ksft_test_result(ret == 0, "get baseline scheduling attributes\n");

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended, "allocate zero-extended sched_attr\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	extended->size = EXTENDED_SIZE;
	errno = 0;
	ret = syscall(__NR_sched_setattr, 0, extended, 0);
	saved_errno = errno;
	ksft_print_msg("extended sched_setattr returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound sched_setattr size to the process page\n");

	memset(extended, 0, EXTENDED_SIZE);
	errno = 0;
	ret = syscall(__NR_sched_getattr, 0, extended, EXTENDED_SIZE, 0);
	saved_errno = errno;
	ksft_print_msg("extended sched_getattr returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound sched_getattr size to the process page\n");
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
	execl("/proc/self/exe", "sched_attr_size_ppps", "--run", NULL);
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
