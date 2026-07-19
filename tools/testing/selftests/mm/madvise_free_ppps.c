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
#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * USER_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * USER_PAGE_SIZE)

static bool vma_stat_bytes(const void *address, const char *name,
			   unsigned long *bytes)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end, value_kb;
	char format[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	FILE *smaps;

	snprintf(format, sizeof(format), "%s: %%lu kB", name);
	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = target >= start && target < end;
			continue;
		}
		if (in_target && sscanf(line, format, &value_kb) == 1) {
			*bytes = value_kb * 1024;
			found = true;
			break;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

static bool vma_span(const void *address, unsigned long *span)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return false;
	while (getline(&line, &capacity, maps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;
		if (target >= start && target < end) {
			*span = end - start;
			found = true;
			break;
		}
	}
	free(line);
	fclose(maps);
	return found;
}

static int run_test(void)
{
	unsigned long lazyfree = 0;
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	bool preserved = true;
	unsigned int i;
	int ret;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mapping = reservation == MAP_FAILED ? MAP_FAILED :
		mmap(reservation + USER_PAGE_SIZE, MAPPING_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping != MAP_FAILED) {
		madvise(mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
		for (i = 0; i < TEST_PAGES; i++)
			mapping[i * USER_PAGE_SIZE] = 0x40 + i;
	}
	ksft_test_result(mapping != MAP_FAILED,
			 "map and populate anonymous process pages\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	ksft_test_result(vma_span(mapping, &span) && span == MAPPING_SIZE,
			 "target is an isolated %lu-byte VMA (%lu bytes)\n",
			 MAPPING_SIZE, span);

	ret = madvise(mapping, MAPPING_SIZE, MADV_FREE);
	ksft_test_result(!ret, "mark the process pages MADV_FREE\n");
	ksft_test_result(vma_stat_bytes(mapping, "LazyFree", &lazyfree) &&
			 lazyfree >= MAPPING_SIZE - USER_PAGE_SIZE,
			 "account lazy-free process pages (%lu bytes)\n",
			 lazyfree);

	for (i = 0; i < TEST_PAGES; i++) {
		if (mapping[i * USER_PAGE_SIZE] != (unsigned char)(0x40 + i)) {
			preserved = false;
			break;
		}
	}
	ksft_test_result(preserved,
			 "lazy-free pages retain data before reclaim\n");

	munmap(reservation, RESERVE_SIZE);
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
	execl("/proc/self/exe", "madvise_free_ppps", "--run", NULL);
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
