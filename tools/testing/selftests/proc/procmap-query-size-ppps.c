// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int query_maps(int fd, struct procmap_query *query)
{
	return ioctl(fd, PROCMAP_QUERY, query);
}

static int run_test(void)
{
	struct procmap_query baseline = {
		.size = sizeof(baseline),
		.query_addr = (uintptr_t)&run_test,
	};
	struct procmap_query *extended;
	int saved_errno;
	int fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open proc maps\n");
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));
	ret = query_maps(fd, &baseline);
	ksft_test_result(ret == 0, "submit a baseline procmap query\n");

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended, "allocate zero-extended procmap_query\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &baseline, sizeof(baseline));
	extended->size = EXTENDED_SIZE;
	errno = 0;
	ret = query_maps(fd, extended);
	saved_errno = errno;
	ksft_print_msg("extended PROCMAP_QUERY returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound procmap_query size to the process page\n");
	free(extended);
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
	execl("/proc/self/exe", "procmap_query_size_ppps", "--run", NULL);
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
