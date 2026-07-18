// SPDX-License-Identifier: GPL-2.0
/*
 * Committed_AS charges and uncharges an anonymous mapping and its mremap
 * expansion in 4K process-page units for a compat process, and strict
 * overcommit enforces its byte limit against such mappings.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "kselftest_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static unsigned long committed_kb(void)
{
	unsigned long value = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		ksft_exit_fail_msg("open /proc/meminfo failed: %s\n",
				   strerror(errno));
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "Committed_AS: %lu kB", &value) == 1)
			break;
	}
	free(line);
	fclose(meminfo);
	if (!value)
		ksft_exit_fail_msg("read Committed_AS failed\n");
	return value;
}

static int run_test(void)
{
	unsigned long before;
	unsigned long after_map;
	unsigned long after_expand;
	long initial_charge;
	long expansion_charge;
	void *mapping;
	void *expanded;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	before = committed_kb();
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one accountable process page\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	after_map = committed_kb();
	initial_charge = after_map - before;
	ksft_test_result(initial_charge > 0,
			 "charge the initial anonymous mapping\n");

	expanded = mremap(mapping, PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
			  MREMAP_MAYMOVE);
	ksft_test_result(expanded != MAP_FAILED,
			 "expand the mapping by one process page\n");
	if (expanded == MAP_FAILED)
		ksft_exit_fail_msg("mremap failed: %s\n", strerror(errno));
	after_expand = committed_kb();
	expansion_charge = after_expand - after_map;
	ksft_test_result(expansion_charge == initial_charge,
			 "charge mremap expansion like the initial process page\n");
	ksft_print_msg("Committed_AS before=%lu mapped=%lu expanded=%lu kB; "
		       "initial=%ld expansion=%ld kB\n",
		       before, after_map, after_expand, initial_charge,
		       expansion_charge);

	munmap(expanded, 2 * USER_PAGE_SIZE);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
