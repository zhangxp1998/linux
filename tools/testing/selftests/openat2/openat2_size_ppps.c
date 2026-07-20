// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_openat2
#define __NR_openat2 437
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int openat2_call(struct open_how *how, size_t size)
{
	return syscall(__NR_openat2, AT_FDCWD, ".", how, size);
}

static int run_test(void)
{
	struct open_how baseline = {
		.flags = O_PATH | O_CLOEXEC,
	};
	struct open_how *extended;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = openat2_call(&baseline, sizeof(baseline));
	ksft_test_result(fd >= 0, "open with a baseline open_how\n");
	if (fd >= 0)
		close(fd);

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended, "allocate a zero-extended open_how\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &baseline, sizeof(baseline));
	errno = 0;
	fd = openat2_call(extended, EXTENDED_SIZE);
	saved_errno = errno;
	ksft_print_msg("extended openat2 returned %d, errno %d (%s)\n", fd,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(fd == -1 && saved_errno == E2BIG,
			 "bound open_how size to the process page\n");
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
	execl("/proc/self/exe", "openat2_size_ppps", "--run", NULL);
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
