// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static long request_invalid_destination(const char *callout)
{
	return syscall(__NR_request_key, "user", "ppps-callout", callout,
		       123456789);
}

static int run_test(void)
{
	char *callout;
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	errno = 0;
	ret = request_invalid_destination("x");
	ksft_test_result(ret == -1 && errno == ENOKEY,
			 "reject an unknown destination after a baseline callout\n");

	callout = malloc(EXTENDED_SIZE);
	ksft_test_result(callout, "allocate a 4096-character callout\n");
	if (!callout)
		ksft_exit_fail_msg("allocation failed\n");
	memset(callout, 'x', USER_PAGE_SIZE);
	callout[USER_PAGE_SIZE] = '\0';
	errno = 0;
	ret = request_invalid_destination(callout);
	saved_errno = errno;
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound request-key callout to the process page\n");
	free(callout);
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
	execl("/proc/self/exe", "request_key_callout_ppps", "--run", NULL);
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
