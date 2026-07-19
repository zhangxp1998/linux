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
#define POLL_ATTEMPTS	200

static bool swap_info(unsigned long *total_bytes, unsigned long *free_bytes)
{
	unsigned long total_kb = 0;
	unsigned long free_kb = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		return false;
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "SwapTotal: %lu kB", &total_kb) == 1)
			continue;
		if (sscanf(line, "SwapFree: %lu kB", &free_kb) == 1)
			continue;
	}
	free(line);
	fclose(meminfo);
	*total_bytes = total_kb * 1024;
	*free_bytes = free_kb * 1024;
	return true;
}

static bool vma_swap_bytes(const void *address, unsigned long *swap_bytes)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end, swap_kb;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	FILE *smaps;

	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = target >= start && target < end;
			continue;
		}
		if (in_target && sscanf(line, "Swap: %lu kB", &swap_kb) == 1) {
			*swap_bytes = swap_kb * 1024;
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

static bool page_out_mapping(void *mapping, unsigned long *swap_bytes)
{
	unsigned long total;
	unsigned long free_before;
	unsigned long free_after;
	unsigned long smaps_bytes;
	unsigned long swap_delta;
	unsigned int attempt;

	if (!swap_info(&total, &free_before))
		return false;
	if (madvise(mapping, MAPPING_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!swap_info(&total, &free_after) ||
		    !vma_swap_bytes(mapping, &smaps_bytes))
			return false;
		swap_delta = free_after < free_before ? free_before - free_after : 0;
		*swap_bytes = smaps_bytes > swap_delta ? smaps_bytes : swap_delta;
		if (*swap_bytes >= MAPPING_SIZE / 2)
			return true;
		usleep(10000);
	}
	return false;
}

static int run_test(void)
{
	unsigned long total_swap = 0;
	unsigned long free_swap = 0;
	unsigned long swapped = 0;
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	bool paged_out;
	bool preserved = true;
	unsigned int i;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (!swap_info(&total_swap, &free_swap))
		ksft_exit_fail_msg("could not read /proc/meminfo\n");
	if (!total_swap)
		ksft_exit_skip("no swap device is active\n");
	ksft_test_result(free_swap, "swap has free space\n");

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

	paged_out = page_out_mapping(mapping, &swapped);
	ksft_test_result(paged_out,
			 "page out at least half of the mapping (%lu bytes)\n",
			 swapped);
	if (!paged_out)
		ksft_exit_fail_msg("could not create swap PTEs\n");

	for (i = 0; i < TEST_PAGES; i++) {
		if (mapping[i * USER_PAGE_SIZE] != (unsigned char)(0x40 + i)) {
			preserved = false;
			break;
		}
	}
	ksft_test_result(preserved,
			 "swapped process pages preserve their contents\n");

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
	execl("/proc/self/exe", "anon_pageout_ppps", "--run", NULL);
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
