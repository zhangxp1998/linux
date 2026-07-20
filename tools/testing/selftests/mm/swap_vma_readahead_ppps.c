// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define ADDRESS_ALIGN	(64 * 1024UL)
#define TEST_PAGES	64
#define TEST_SIZE	(TEST_PAGES * USER_PAGE_SIZE)
#define RESERVE_SIZE	(TEST_SIZE + 2 * ADDRESS_ALIGN)
#define POLL_ATTEMPTS	300

static const char *page_cluster_path = "/proc/sys/vm/page-cluster";
static const char *vma_ra_path = "/sys/kernel/mm/swap/vma_ra_enabled";
static unsigned long saved_page_cluster;
static char saved_vma_ra[16];
static bool restore_page_cluster;
static bool restore_vma_ra;

static bool read_named_value(const char *path, const char *name,
			     unsigned long *value)
{
	char key[64];
	unsigned long current;
	char *line = NULL;
	size_t capacity = 0;
	FILE *file;

	file = fopen(path, "re");
	if (!file)
		return false;
	while (getline(&line, &capacity, file) >= 0) {
		if (sscanf(line, "%63s %lu", key, &current) == 2 &&
		    !strcmp(key, name)) {
			*value = current;
			free(line);
			fclose(file);
			return true;
		}
	}
	free(line);
	fclose(file);
	return false;
}

static bool read_ulong_file(const char *path, unsigned long *value)
{
	FILE *file = fopen(path, "re");
	bool success = file && fscanf(file, "%lu", value) == 1;

	if (file)
		fclose(file);
	return success;
}

static bool read_word_file(const char *path, char *word, size_t size)
{
	FILE *file = fopen(path, "re");
	bool success = file && fgets(word, size, file);

	if (file)
		fclose(file);
	if (success)
		word[strcspn(word, "\n")] = '\0';
	return success && word[0];
}

static bool write_text_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "we");
	bool success;

	if (!file)
		return false;
	success = fputs(text, file) >= 0;
	if (fclose(file))
		success = false;
	return success;
}

static void restore_readahead_config(void)
{
	char value[32];

	if (restore_page_cluster) {
		snprintf(value, sizeof(value), "%lu\n", saved_page_cluster);
		write_text_file(page_cluster_path, value);
	}
	if (restore_vma_ra)
		write_text_file(vma_ra_path, saved_vma_ra);
}

static bool mapping_usage(const void *address, unsigned long *rss_bytes,
			  unsigned long *swap_bytes)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end, value_kb;
	char *line = NULL;
	size_t capacity = 0;
	bool found_rss = false;
	bool found_swap = false;
	bool in_target = false;
	FILE *smaps;

	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_target)
				break;
			in_target = target >= start && target < end;
			continue;
		}
		if (!in_target)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found_rss = true;
		} else if (sscanf(line, "Swap: %lu kB", &value_kb) == 1) {
			*swap_bytes = value_kb * 1024;
			found_swap = true;
		}
	}
	free(line);
	fclose(smaps);
	return found_rss && found_swap;
}

static bool mapping_span(const void *address, unsigned long *span)
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

static bool page_out_mapping(void *mapping, unsigned long *rss_bytes,
			     unsigned long *swap_bytes)
{
	unsigned int attempt;

	if (madvise(mapping, TEST_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!mapping_usage(mapping, rss_bytes, swap_bytes))
			return false;
		if (*swap_bytes >= TEST_SIZE / 2 && *rss_bytes <= TEST_SIZE / 2)
			return true;
		usleep(10000);
	}
	return false;
}

static bool apply_memory_pressure(void)
{
	unsigned long available_kb = 0;
	unsigned long kernel_page_kb = 0;
	unsigned long allocation_factor;
	size_t pressure_size;
	unsigned char *pressure;
	size_t offset;

	if (!read_named_value("/proc/meminfo", "MemAvailable:",
			      &available_kb) || available_kb < 128 * 1024 ||
	    !read_named_value("/proc/self/smaps", "KernelPageSize:",
			      &kernel_page_kb))
		return false;
	allocation_factor = kernel_page_kb * 1024 / USER_PAGE_SIZE;
	if (!allocation_factor)
		allocation_factor = 1;
	pressure_size = (available_kb + 32 * 1024) * 1024 /
		allocation_factor;
	if (pressure_size > 512UL * 1024 * 1024 / allocation_factor)
		pressure_size = 512UL * 1024 * 1024 / allocation_factor;
	pressure = mmap(NULL, pressure_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pressure == MAP_FAILED)
		return false;
	madvise((void *)pressure, pressure_size, MADV_NOHUGEPAGE);
	for (offset = 0; offset < pressure_size; offset += USER_PAGE_SIZE)
		pressure[offset] = offset / USER_PAGE_SIZE;
	munmap((void *)pressure, pressure_size);
	return true;
}

static long major_faults(void)
{
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage))
		return -1;
	return usage.ru_majflt;
}

static bool fault_and_check(unsigned char *mapping, unsigned int page,
			    long *major_delta)
{
	long before = major_faults();
	unsigned char value = mapping[page * USER_PAGE_SIZE];
	long after = major_faults();

	if (before < 0 || after < 0)
		return false;
	*major_delta = after - before;
	return value == (unsigned char)(0x40 + page);
}

static int run_test(void)
{
	unsigned long total_swap_kb = 0;
	unsigned long rss_bytes = 0;
	unsigned long swap_bytes = 0;
	unsigned long span = 0;
	unsigned long swap_ra_before = 0;
	unsigned long swap_ra_after = 0;
	unsigned char *reservation;
	unsigned char *mapping;
	uintptr_t aligned;
	long major_delta[3] = {};
	bool configured;
	bool mapped;
	bool paged_out;
	bool preserved[3];
	unsigned int i;

	ksft_print_header();
	ksft_set_plan(12);
	if (atexit(restore_readahead_config))
		ksft_exit_fail_msg("could not register configuration cleanup\n");
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (sysconf(_SC_PAGESIZE) != USER_PAGE_SIZE)
		ksft_exit_fail_msg("could not enter 4K process mode\n");

	if (!read_named_value("/proc/meminfo", "SwapTotal:",
			      &total_swap_kb) || !total_swap_kb)
		ksft_exit_skip("active swap is required\n");
	ksft_test_result_pass("swap is active\n");

	restore_page_cluster = read_ulong_file(page_cluster_path,
					       &saved_page_cluster);
	restore_vma_ra = read_word_file(vma_ra_path, saved_vma_ra,
					sizeof(saved_vma_ra));
	configured = restore_page_cluster && restore_vma_ra &&
		write_text_file(page_cluster_path, "1\n") &&
		write_text_file(vma_ra_path, "true\n");
	ksft_test_result(configured,
			 "enable a two-page VMA readahead window\n");
	if (!configured)
		ksft_exit_fail_msg("could not configure swap readahead\n");

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	aligned = reservation == MAP_FAILED ? 0 :
		((uintptr_t)reservation + 2 * ADDRESS_ALIGN - 1) &
		~(uintptr_t)(ADDRESS_ALIGN - 1);
	mapping = aligned ? mmap((void *)aligned, TEST_SIZE,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
				 -1, 0) : MAP_FAILED;
	mapped = mapping != MAP_FAILED && !(aligned & (ADDRESS_ALIGN - 1)) &&
		madvise((void *)mapping, TEST_SIZE, MADV_NOHUGEPAGE) == 0 &&
		mapping_span((const void *)mapping, &span) && span == TEST_SIZE;
	ksft_test_result(mapped,
			 "map an isolated native-aligned 4K-page VMA\n");
	if (!mapped)
		ksft_exit_fail_msg("could not create test mapping\n");

	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * USER_PAGE_SIZE] = 0x40 + i;
	paged_out = page_out_mapping((void *)mapping, &rss_bytes, &swap_bytes);
	ksft_test_result(paged_out,
			 "page out the VMA (Swap: %lu, Rss: %lu bytes)\n",
			 swap_bytes, rss_bytes);
	if (!paged_out)
		ksft_exit_fail_msg("could not page out test mapping\n");
	if (!apply_memory_pressure())
		ksft_exit_fail_msg("could not reclaim swapped folios\n");
	ksft_print_msg("applied memory pressure to evict swap cache\n");

	preserved[0] = fault_and_check(mapping, 0, &major_delta[0]);
	ksft_test_result(preserved[0], "fault page 0 with intact contents\n");
	ksft_test_result(major_delta[0] > 0,
			 "page 0 requires swap I/O\n");

	preserved[1] = fault_and_check(mapping, 2, &major_delta[1]);
	ksft_test_result(preserved[1], "fault nonadjacent page 2 with intact contents\n");
	ksft_test_result(major_delta[1] > 0,
			 "page 2 requires swap I/O and resets history\n");

	if (!read_named_value("/proc/vmstat", "swap_ra", &swap_ra_before))
		ksft_exit_fail_msg("could not read swap_ra before fault\n");
	preserved[2] = fault_and_check(mapping, 3, &major_delta[2]);
	if (!read_named_value("/proc/vmstat", "swap_ra", &swap_ra_after))
		ksft_exit_fail_msg("could not read swap_ra after fault\n");
	ksft_test_result(preserved[2], "fault adjacent page 3 with intact contents\n");
	ksft_test_result(major_delta[2] > 0,
			 "page 3 requires swap I/O\n");
	ksft_test_result(swap_ra_after > swap_ra_before,
			 "adjacent 4K fault starts VMA readahead # before %lu after %lu\n",
			 swap_ra_before, swap_ra_after);

	munmap(reservation, RESERVE_SIZE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	int persona;

	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	if (argc != 1)
		return EXIT_FAILURE;

	persona = personality(0xffffffffUL);
	if (persona < 0 ||
	    personality((unsigned long)persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality failed: %s\n", strerror(errno));
	execl("/proc/self/exe", "swap_vma_readahead_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}
