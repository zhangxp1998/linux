// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE		4096UL
#define NATIVE_PAGE_SIZE	16384UL
#define MAPPING_SIZE		(8 * USER_PAGE_SIZE)
#define UNMAP_SIZE		(3 * USER_PAGE_SIZE)

static bool is_mapped(void *address)
{
	unsigned char vector;

	errno = 0;
	return !mincore(address, USER_PAGE_SIZE, &vector);
}

static int run_test(void)
{
	unsigned char *mapping;
	unsigned char *aligned;
	unsigned int page;
	bool first_three_unmapped = true;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, MAPPING_SIZE + NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "create an anonymous mapping\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	aligned = (unsigned char *)(((uintptr_t)mapping +
				    NATIVE_PAGE_SIZE - 1) &
				   ~(NATIVE_PAGE_SIZE - 1));
	for (page = 0; page < 4; page++)
		aligned[page * USER_PAGE_SIZE] = 0x40 + page;

	ksft_test_result(!munmap(aligned, UNMAP_SIZE),
			 "unmap exactly three 4K process pages\n");

	for (page = 0; page < 3; page++)
		first_three_unmapped &= !is_mapped(aligned +
						  page * USER_PAGE_SIZE);
	ksft_test_result(first_three_unmapped,
			 "the requested three pages are unmapped\n");

	ksft_test_result(is_mapped(aligned + 3 * USER_PAGE_SIZE) &&
			 aligned[3 * USER_PAGE_SIZE] == 0x43,
			 "munmap does not remove the adjacent fourth page\n");

	munmap(mapping, MAPPING_SIZE + NATIVE_PAGE_SIZE);
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
	execl("/proc/self/exe", "munmap_ppps", "--run", NULL);
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
