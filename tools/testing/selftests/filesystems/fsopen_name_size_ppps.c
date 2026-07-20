// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif

#define USER_PAGE_SIZE 4096UL

static int run_test(void)
{
	char *name;
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	name = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(name != MAP_FAILED, "map two process pages\n");
	if (name == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(name, 'x', USER_PAGE_SIZE);
	ret = mprotect(name + USER_PAGE_SIZE, USER_PAGE_SIZE, PROT_NONE);
	ksft_test_result(ret == 0,
			 "protect the page after the filesystem name\n");
	if (ret)
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	errno = 0;
	ret = syscall(__NR_fsopen, name, 0);
	saved_errno = errno;
	ksft_print_msg("fsopen returned %ld, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	if (ret >= 0)
		close(ret);
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound filesystem names to one process page\n");
	munmap(name, 2 * USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "fsopen_name_size_ppps", "--run", NULL);
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
