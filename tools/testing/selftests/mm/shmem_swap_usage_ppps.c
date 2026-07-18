// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define VMA_SIZE	(4 * USER_PAGE_SIZE)
#define RESERVE_SIZE	(16 * USER_PAGE_SIZE)
#define FILE_SIZE	(16 * USER_PAGE_SIZE)
#define POLL_ATTEMPTS	100

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

static bool page_out_mapping(void *mapping, unsigned long *swap_delta)
{
	unsigned long free_before;
	unsigned long free_after;
	unsigned long total;
	unsigned int attempt;

	if (!swap_info(&total, &free_before))
		return false;
	if (madvise(mapping, VMA_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!swap_info(&total, &free_after))
			return false;
		if (free_after < free_before) {
			*swap_delta = free_before - free_after;
			return true;
		}
		usleep(10000);
	}
	return false;
}

static int run_test(void)
{
	unsigned long total_swap;
	unsigned long free_swap;
	unsigned long outside_swap = 0;
	unsigned long swap_delta = 0;
	unsigned long target_swap = 0;
	unsigned long slice_swap = 0;
	unsigned char *reservation;
	unsigned char *outside;
	unsigned char *target;
	bool paged_out;
	size_t offset;
	int fd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (!swap_info(&total_swap, &free_swap))
		ksft_exit_fail_msg("could not read /proc/meminfo\n");
	if (!total_swap)
		ksft_exit_skip("no swap device is active\n");
	ksft_test_result(true, "swap is active\n");

	fd = memfd_create("shmem-swap-usage-ppps", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, FILE_SIZE))
		ksft_exit_fail_msg("create memfd failed: %s\n", strerror(errno));
	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("reserve address range failed: %s\n",
				   strerror(errno));
	target = mmap(reservation, VMA_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, 0);
	outside = mmap(reservation + 2 * VMA_SIZE, VMA_SIZE,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		       fd, VMA_SIZE);
	if (target == MAP_FAILED || outside == MAP_FAILED)
		ksft_exit_fail_msg("map shmem ranges failed: %s\n",
				   strerror(errno));
	ksft_test_result(true, "map separate inside and outside shmem ranges\n");

	for (offset = 0; offset < VMA_SIZE; offset += USER_PAGE_SIZE)
		outside[offset] = 0x40 + offset / USER_PAGE_SIZE;
	paged_out = page_out_mapping(outside, &swap_delta);
	ksft_test_result(paged_out, "page out only the range outside the target VMA\n");
	if (!vma_swap_bytes(outside, &outside_swap))
		ksft_exit_fail_msg("could not read outside VMA swap usage\n");
	if (!vma_swap_bytes(target, &target_swap))
		ksft_exit_fail_msg("could not read target VMA swap usage\n");
	ksft_test_result(paged_out && !target_swap,
			 "exclude swapped shmem pages outside the target VMA\n");
	ksft_print_msg("swap delta=%lu outside Swap=%lu target Swap=%lu bytes\n",
		       swap_delta, outside_swap, target_swap);

	munmap(outside, VMA_SIZE);
	outside = mmap(reservation + 2 * VMA_SIZE, USER_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		       fd, VMA_SIZE);
	if (outside == MAP_FAILED)
		ksft_exit_fail_msg("map swapped shmem slice failed: %s\n",
				   strerror(errno));
	if (!vma_swap_bytes(outside, &slice_swap))
		ksft_exit_fail_msg("could not read shmem slice swap usage\n");
	ksft_test_result(slice_swap == USER_PAGE_SIZE,
			 "account only the mapped process-page shmem slice\n");
	ksft_print_msg("single-slice Swap=%lu bytes\n", slice_swap);

	munmap(reservation, RESERVE_SIZE);
	close(fd);
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
	execl("/proc/self/exe", "shmem_swap_usage_ppps", "--run", NULL);
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
