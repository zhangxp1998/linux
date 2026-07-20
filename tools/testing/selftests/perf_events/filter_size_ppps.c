// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/perf_event.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static int open_event(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_CPU_CLOCK,
		.disabled = 1,
	};

	return syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}

static int run_test(void)
{
	char *filter;
	int saved_errno;
	int fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open_event();
	ksft_test_result(fd >= 0, "create a software perf event\n");
	if (fd < 0)
		ksft_exit_fail_msg("perf_event_open failed: %s\n",
				   strerror(errno));

	filter = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(filter != MAP_FAILED, "map two process pages\n");
	if (filter == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(filter, 'x', USER_PAGE_SIZE);
	ret = mprotect(filter + USER_PAGE_SIZE, USER_PAGE_SIZE, PROT_NONE);
	ksft_test_result(ret == 0, "protect the page after the filter\n");
	if (ret)
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	errno = 0;
	ret = ioctl(fd, PERF_EVENT_IOC_SET_FILTER, filter);
	saved_errno = errno;
	ksft_print_msg("filter ioctl returned %d, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound perf filters to one process page\n");
	munmap(filter, 2 * USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "perf_filter_size_ppps", "--run", NULL);
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
