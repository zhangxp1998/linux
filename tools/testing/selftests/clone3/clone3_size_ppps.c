// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_clone3
#define __NR_clone3 435
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static long clone_and_reap(struct clone_args *args, size_t size,
			   int *saved_errno)
{
	long ret;
	int status;

	errno = 0;
	ret = syscall(__NR_clone3, args, size);
	*saved_errno = errno;
	if (ret == 0)
		_exit(EXIT_SUCCESS);
	if (ret > 0 && waitpid(ret, &status, 0) != ret)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	return ret;
}

static int run_test(void)
{
	struct clone_args baseline = {
		.exit_signal = SIGCHLD,
	};
	struct clone_args *extended;
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ret = clone_and_reap(&baseline, sizeof(baseline), &saved_errno);
	ksft_test_result(ret > 0, "clone with baseline clone_args\n");

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended, "allocate zero-extended clone_args\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &baseline, sizeof(baseline));
	ret = clone_and_reap(extended, EXTENDED_SIZE, &saved_errno);
	ksft_print_msg("extended clone3 returned %ld, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound clone_args size to the process page\n");
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
	execl("/proc/self/exe", "clone3_size_ppps", "--run", NULL);
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
