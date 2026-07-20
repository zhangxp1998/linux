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
#define TEST_GAP	(2UL * 1024 * 1024)
#define RESERVE_SIZE	(TEST_GAP + 2 * USER_PAGE_SIZE)

static int run_test(void)
{
	unsigned char *reservation;
	unsigned char *stack_addr;
	void *mapping;
	void *stack;
	bool reserved;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	reserved = reservation != MAP_FAILED;
	ksft_test_result(reserved, "reserve an isolated address range\n");
	if (!reserved)
		ksft_exit_fail_msg("reservation mmap failed: %s\n",
				   strerror(errno));
	stack_addr = reservation + TEST_GAP + USER_PAGE_SIZE;
	if (munmap(reservation, RESERVE_SIZE))
		ksft_exit_fail_msg("reservation munmap failed: %s\n",
				   strerror(errno));

	stack = mmap(stack_addr, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE |
		     MAP_GROWSDOWN, -1, 0);
	ksft_test_result(stack == stack_addr, "create a grow-down VMA\n");
	if (stack == MAP_FAILED || stack != stack_addr)
		ksft_exit_fail_msg("grow-down mmap failed: %s\n",
				   strerror(errno));

	mapping = mmap(reservation, USER_PAGE_SIZE, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map below the grow-down VMA\n");
	ksft_test_result(mapping == reservation,
			 "2 MiB gap accepts a 4K process mmap hint\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, USER_PAGE_SIZE);
	munmap(stack, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "stack_guard_gap_ppps", "--run", NULL);
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
