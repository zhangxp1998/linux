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

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

#define USER_PAGE_SIZE 4096UL
#define TEST_LENGTH (4 * USER_PAGE_SIZE)
#define SOURCE_ADDRESS ((void *)0x40001000UL)
#define HINT_ADDRESS ((void *)0x50000000UL)

static int run_test(void)
{
	unsigned char *source = SOURCE_ADDRESS;
	unsigned char *hint = HINT_ADDRESS;
	unsigned char *moved;
	bool moved_data_ok = true;
	bool source_zeroed = true;
	unsigned int i;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	source = mmap(source, TEST_LENGTH, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (source == MAP_FAILED)
		ksft_exit_fail_msg("source mmap failed: %s\n", strerror(errno));
	for (i = 0; i < TEST_LENGTH; i++)
		source[i] = i / USER_PAGE_SIZE + 0x31;

	/* Reserve and release the range to prove the requested hint is free. */
	hint = mmap(hint, TEST_LENGTH, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (hint == MAP_FAILED)
		ksft_exit_fail_msg("hint mmap failed: %s\n", strerror(errno));
	if (munmap(hint, TEST_LENGTH))
		ksft_exit_fail_msg("hint munmap failed: %s\n", strerror(errno));

	moved = mremap(source, TEST_LENGTH, TEST_LENGTH,
		       MREMAP_MAYMOVE | MREMAP_DONTUNMAP, hint);
	if (moved == MAP_FAILED)
		ksft_exit_fail_msg("mremap failed: %s\n", strerror(errno));
	ksft_test_result(moved == hint,
			 "MREMAP_DONTUNMAP honors an available hint\n");

	for (i = 0; i < TEST_LENGTH; i++) {
		if (moved[i] != (unsigned char)(i / USER_PAGE_SIZE + 0x31))
			moved_data_ok = false;
		if (source[i])
			source_zeroed = false;
	}
	ksft_test_result(moved_data_ok,
			 "cross-slice move preserves all process pages\n");
	ksft_test_result(source_zeroed,
			 "MREMAP_DONTUNMAP leaves zero-filled source pages\n");

	munmap(moved, TEST_LENGTH);
	munmap(source, TEST_LENGTH);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n", strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n", strerror(errno));
	execl("/proc/self/exe", "mremap_hint_ppps", "--run", NULL);
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
