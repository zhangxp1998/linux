// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/mempolicy.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE 16384UL

static int run_test(void)
{
	unsigned char *mapping;
	unsigned char *target;
	unsigned long nodemask = 1;
	bool aligned;
	long result;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	target = mapping + USER_PAGE_SIZE;
	aligned = !((unsigned long)target & (USER_PAGE_SIZE - 1)) &&
		  ((unsigned long)target & (NATIVE_PAGE_SIZE - 1));
	ksft_test_result(aligned,
			 "target is 4K aligned but not 16K aligned\n");
	target[0] = 0xa5;

	errno = 0;
	result = syscall(__NR_mbind, target, USER_PAGE_SIZE, MPOL_DEFAULT,
			 NULL, 0, 0);
	ksft_test_result(!result, "apply MPOL_DEFAULT to one 4K subpage\n");
	ksft_print_msg("MPOL_DEFAULT result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_mbind, target, USER_PAGE_SIZE, MPOL_BIND,
			 &nodemask, sizeof(nodemask) * 8, 0);
	ksft_test_result(!result, "bind one 4K subpage to node 0\n");
	ksft_print_msg("MPOL_BIND result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_set_mempolicy_home_node, target, USER_PAGE_SIZE,
			 0, 0);
	ksft_test_result(!result, "set home node on one 4K subpage\n");
	ksft_print_msg("home-node result=%ld errno=%d\n", result, errno);

	munmap(mapping, NATIVE_PAGE_SIZE);
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
	execl("/proc/self/exe", "mbind_ppps", "--run", NULL);
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
