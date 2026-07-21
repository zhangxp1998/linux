// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_file_getattr
#define __NR_file_getattr 468
#endif

#ifndef __NR_file_setattr
#define __NR_file_setattr 469
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct file_attr *attr;
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	attr = calloc(1, EXTENDED_SIZE);
	ksft_test_result(attr, "allocate zero-extended file_attr\n");
	if (!attr)
		ksft_exit_fail_msg("allocation failed\n");

	errno = 0;
	ret = syscall(__NR_file_getattr, AT_FDCWD, ".", attr,
		      EXTENDED_SIZE, 0);
	saved_errno = errno;
	ksft_print_msg("extended file_getattr returned %ld, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound file_getattr arguments to the process page\n");

	memset(attr, 0, EXTENDED_SIZE);
	errno = 0;
	ret = syscall(__NR_file_setattr, AT_FDCWD, ".", attr,
		      EXTENDED_SIZE, 0);
	saved_errno = errno;
	ksft_print_msg("extended file_setattr returned %ld, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound file_setattr arguments to the process page\n");
	free(attr);
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
	execl("/proc/self/exe", "file_attr_size_ppps", "--run", NULL);
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
