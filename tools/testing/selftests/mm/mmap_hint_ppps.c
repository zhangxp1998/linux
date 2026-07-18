// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define RESERVE_SIZE	(4 * USER_PAGE_SIZE)

static int run_test(void)
{
	unsigned char *reservation;
	unsigned char *mapping;
	unsigned char *hint;
	bool reservation_ok;
	bool hint_honored;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	reservation_ok = reservation != MAP_FAILED;
	ksft_test_result(reservation_ok, "reserve a known free address range\n");
	if (!reservation_ok)
		ksft_exit_fail_msg("reservation mmap failed: %s\n",
				   strerror(errno));
	hint = reservation + USER_PAGE_SIZE;
	if (munmap(reservation, RESERVE_SIZE))
		ksft_exit_fail_msg("reservation munmap failed: %s\n",
				   strerror(errno));

	mapping = mmap(hint, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	hint_honored = mapping == hint;
	ksft_test_result(hint_honored,
			 "mmap honors an available 4K-only-aligned hint\n");
	if (!hint_honored)
		ksft_print_msg("requested hint=%p, received=%p\n", hint, mapping);

	if (mapping != MAP_FAILED) {
		mapping[0] = 0x5a;
		ksft_test_result(mapping[0] == 0x5a,
				 "mapping returned for the hint is usable\n");
		munmap(mapping, USER_PAGE_SIZE);
	} else {
		ksft_test_result_fail("mapping returned for the hint is usable\n");
	}
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
	execl("/proc/self/exe", "mmap_hint_ppps", "--run", NULL);
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
