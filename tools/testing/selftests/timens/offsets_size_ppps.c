// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static ssize_t write_offsets(const void *buf, size_t size)
{
	int fd = open("/proc/self/timens_offsets", O_WRONLY | O_CLOEXEC);
	ssize_t ret;

	if (fd < 0)
		return -1;
	ret = write(fd, buf, size);
	close(fd);
	return ret;
}

static int run_test(void)
{
	static const char baseline[] = "monotonic 0 0\n";
	char *extended;
	int saved_errno;
	ssize_t ret;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ksft_test_result(unshare(CLONE_NEWTIME) == 0,
			 "create a time namespace\n");
	ret = write_offsets(baseline, sizeof(baseline) - 1);
	ksft_test_result(ret == (ssize_t)sizeof(baseline) - 1,
			 "write a baseline time namespace offset\n");

	extended = calloc(1, USER_PAGE_SIZE);
	ksft_test_result(extended, "allocate a 4096-byte offset record\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, baseline, sizeof(baseline));
	errno = 0;
	ret = write_offsets(extended, USER_PAGE_SIZE);
	saved_errno = errno;
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound writes to less than one process page\n");
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
	execl("/proc/self/exe", "timens_offsets_size_ppps", "--run", NULL);
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
