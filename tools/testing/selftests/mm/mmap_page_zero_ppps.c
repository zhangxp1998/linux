// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static int read_zero_mapping(unsigned long *endp, char perms[5])
{
	char line[256];
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return -1;
	while (fgets(line, sizeof(line), maps)) {
		unsigned long start, end;

		if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3 &&
		    start == 0) {
			*endp = end;
			fclose(maps);
			return 0;
		}
	}
	fclose(maps);
	return -1;
}

static int run_test(void)
{
	unsigned long page_size = sysconf(_SC_PAGESIZE);
	unsigned long end = 0;
	char perms[5] = {};
	bool found;

	found = read_zero_mapping(&end, perms) == 0;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(page_size == USER_PAGE_SIZE,
			 "exec selected a 4K process page size\n");
	ksft_test_result(found && end == page_size,
			 "MMAP_PAGE_ZERO maps one process page\n");
	ksft_print_msg("process page size %lu, page-zero mapping 0-%lx %s\n",
		       page_size, end, found ? perms : "missing");
	ksft_finished();
}

int main(int argc, char **argv)
{
	int persona;

	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	if (argc != 1)
		ksft_exit_fail_msg("unexpected arguments\n");

	persona = personality(0xffffffffUL);
	if (persona < 0)
		ksft_exit_fail_msg("failed to read personality\n");
	persona |= MMAP_PAGE_ZERO | ADDR_4KB_COMPAT_PAGE_SIZE;
	if (personality((unsigned int)persona) < 0)
		ksft_exit_fail_msg("failed to set personality\n");

	execl("/proc/self/exe", "mmap_page_zero_ppps", "--run", NULL);
	ksft_exit_fail_msg("failed to exec test process\n");
}
