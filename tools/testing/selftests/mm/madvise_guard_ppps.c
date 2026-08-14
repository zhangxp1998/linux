// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif
#ifndef MADV_GUARD_INSTALL
#define MADV_GUARD_INSTALL 102
#endif
#ifndef MADV_GUARD_REMOVE
#define MADV_GUARD_REMOVE 103
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_SIZE	(4 * USER_PAGE_SIZE)

static bool read_faults(const unsigned char *address)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return false;
	if (!child) {
		unsigned char value = *address;

		_exit(value == 0xff ? EXIT_FAILURE : EXIT_SUCCESS);
	}
	if (waitpid(child, &status, 0) != child)
		return false;
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

static int run_test(void)
{
	unsigned char *mapping;
	bool installed;
	bool removed;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(mapping, 0x5a, MAPPING_SIZE);

	installed = !madvise(mapping + USER_PAGE_SIZE, 3 * USER_PAGE_SIZE,
			     MADV_GUARD_INSTALL);
	ksft_test_result(installed,
			 "install three process-page guard markers\n");
	ksft_test_result(mapping[0] == 0x5a,
			 "unadvised sibling remains accessible\n");
	ksft_test_result(read_faults(mapping + 2 * USER_PAGE_SIZE),
			 "middle process-page guard faults\n");

	removed = !madvise(mapping + USER_PAGE_SIZE, 3 * USER_PAGE_SIZE,
			   MADV_GUARD_REMOVE);
	ksft_test_result(removed, "remove three process-page guards\n");
	ksft_test_result(mapping[USER_PAGE_SIZE] == 0 &&
			 mapping[2 * USER_PAGE_SIZE] == 0 &&
			 mapping[3 * USER_PAGE_SIZE] == 0,
			 "removed guards fault back as zero pages\n");

	munmap(mapping, MAPPING_SIZE);
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
	execl("/proc/self/exe", "madvise_guard_ppps", "--run", NULL);
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
