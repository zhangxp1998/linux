// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL

static unsigned long current_vm_pages(void)
{
	unsigned long pages;
	FILE *statm = fopen("/proc/self/statm", "r");

	if (!statm)
		ksft_exit_fail_msg("open statm failed: %s\n", strerror(errno));
	if (fscanf(statm, "%lu", &pages) != 1)
		ksft_exit_fail_msg("read statm failed\n");
	fclose(statm);
	return pages;
}

static int run_test(void)
{
	struct rlimit limit;
	unsigned long vm_pages;
	void *mapping;
	void *result;
	bool limit_set;
	bool rejected;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	vm_pages = current_vm_pages();
	limit.rlim_cur = vm_pages * USER_PAGE_SIZE;
	limit.rlim_max = limit.rlim_cur;
	limit_set = !setrlimit(RLIMIT_AS, &limit);
	ksft_test_result(limit_set, "limit address space to its current size\n");
	if (!limit_set)
		ksft_exit_fail_msg("setrlimit failed: %s\n", strerror(errno));

	errno = 0;
	result = mremap(mapping, USER_PAGE_SIZE, 2 * USER_PAGE_SIZE,
			MREMAP_MAYMOVE);
	rejected = result == MAP_FAILED && errno == ENOMEM;
	ksft_test_result(rejected,
			 "reject a one-page mremap expansion at RLIMIT_AS\n");
	ksft_print_msg("statm_pages=%lu limit=%llu mremap=%p errno=%d\n",
		       vm_pages, (unsigned long long)limit.rlim_cur, result, errno);

	if (result != MAP_FAILED)
		munmap(result, 2 * USER_PAGE_SIZE);
	else
		munmap(mapping, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "mremap_rlimit_ppps", "--run", NULL);
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
