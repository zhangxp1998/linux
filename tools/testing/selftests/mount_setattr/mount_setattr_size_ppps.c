// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int mount_setattr_call(struct mount_attr *attr, size_t size)
{
	return syscall(__NR_mount_setattr, AT_FDCWD, ".", 0, attr, size);
}

static int run_test(void)
{
	struct mount_attr baseline = {};
	struct mount_attr *extended;
	int saved_errno;
	int ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ret = mount_setattr_call(&baseline, sizeof(baseline));
	ksft_test_result(ret == 0, "submit a baseline no-op mount_attr\n");

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended, "allocate zero-extended mount_attr\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	errno = 0;
	ret = mount_setattr_call(extended, EXTENDED_SIZE);
	saved_errno = errno;
	ksft_print_msg("extended mount_setattr returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound mount_attr size to the process page\n");
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
	execl("/proc/self/exe", "mount_setattr_size_ppps", "--run", NULL);
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
