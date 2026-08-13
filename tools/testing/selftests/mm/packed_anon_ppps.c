// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define PROCESS_PAGE	4096UL
#define NATIVE_PAGE	16384UL
#define SLICES		(NATIVE_PAGE / PROCESS_PAGE)
#define PAGEMAP_PRESENT	UINT64_C(0x8000000000000000)
#define PAGEMAP_SWAPPED	UINT64_C(0x4000000000000000)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define DISCARD_GROUPS	8

static unsigned char *map_aligned(size_t size, unsigned char **reservation)
{
	unsigned char *mapping;
	uintptr_t aligned;

	mapping = mmap(NULL, size + NATIVE_PAGE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)mapping + NATIVE_PAGE - 1) &
		  ~(NATIVE_PAGE - 1);
	*reservation = mapping;
	return (unsigned char *)aligned;
}

static void populate(unsigned char *base)
{
	unsigned int i;

	for (i = 0; i < SLICES; i++)
		memset(base + i * PROCESS_PAGE, 0x31 + i, PROCESS_PAGE);
}

static bool verify(const unsigned char *base)
{
	unsigned long offset;

	for (offset = 0; offset < NATIVE_PAGE; offset++)
		if (base[offset] != 0x31 + offset / PROCESS_PAGE)
			return false;
	return true;
}

static bool all_zero(const unsigned char *base)
{
	unsigned long offset;

	for (offset = 0; offset < NATIVE_PAGE; offset++)
		if (base[offset])
			return false;
	return true;
}

static bool range_stat_bytes(const void *address, unsigned long length,
			     const char *name, unsigned long *bytes)
{
	unsigned long target_start = (unsigned long)address;
	unsigned long target_end = target_start + length;
	unsigned long start, end, value_kb;
	char format[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	FILE *smaps;

	*bytes = 0;
	snprintf(format, sizeof(format), "%s: %%lu kB", name);
	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = target_start < end && target_end > start;
			continue;
		}
		if (in_target && sscanf(line, format, &value_kb) == 1) {
			*bytes += value_kb * 1024;
			found = true;
			in_target = false;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

static bool read_pfns(const unsigned char *base, uint64_t pfn[SLICES])
{
	off_t offset;
	uint64_t entry;
	int fd;
	unsigned int i;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	for (i = 0; i < SLICES; i++) {
		offset = ((uintptr_t)(base + i * PROCESS_PAGE) / PROCESS_PAGE) *
			 sizeof(entry);
		if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
		    !(entry & PAGEMAP_PRESENT)) {
			close(fd);
			return false;
		}
		pfn[i] = entry & PAGEMAP_PFN_MASK;
	}
	close(fd);
	return true;
}

static bool same_pfn(const uint64_t pfn[SLICES])
{
	unsigned int i;

	if (!pfn[0])
		return false;
	for (i = 1; i < SLICES; i++)
		if (pfn[i] != pfn[0])
			return false;
	return true;
}

static bool same_swap_entry(const unsigned char *base)
{
	uint64_t first = 0;
	uint64_t entry;
	off_t offset;
	int fd;
	unsigned int i;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	for (i = 0; i < SLICES; i++) {
		offset = ((uintptr_t)(base + i * PROCESS_PAGE) / PROCESS_PAGE) *
			 sizeof(entry);
		if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
		    !(entry & PAGEMAP_SWAPPED) || (entry & PAGEMAP_PRESENT)) {
			close(fd);
			return false;
		}
		entry &= PAGEMAP_PFN_MASK;
		if (i && entry != first) {
			close(fd);
			return false;
		}
		first = entry;
	}
	close(fd);
	return true;
}

static bool distinct_pfns(const uint64_t pfn[SLICES])
{
	unsigned int i, j;

	for (i = 0; i < SLICES; i++) {
		if (!pfn[i])
			return false;
		for (j = i + 1; j < SLICES; j++)
			if (pfn[i] == pfn[j])
				return false;
	}
	return true;
}

static bool all_groups_swapped(const unsigned char *base, unsigned int groups)
{
	unsigned int i;

	for (i = 0; i < groups; i++)
		if (!same_swap_entry(base + i * NATIVE_PAGE))
			return false;
	return true;
}

static bool fork_cow_preserves_parent(unsigned char *base)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		base[PROCESS_PAGE] = 0xa5;
		if (base[PROCESS_PAGE] != 0xa5 ||
		    base[0] != 0x31 || base[2 * PROCESS_PAGE] != 0x33 ||
		    base[3 * PROCESS_PAGE] != 0x34)
			_exit(1);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child)
		return false;
	return WIFEXITED(status) && !WEXITSTATUS(status) && verify(base);
}

struct concurrent_write_args {
	unsigned char *base;
	unsigned int slice;
	atomic_bool *start;
};

static void *write_zero_tuple_slice(void *data)
{
	struct concurrent_write_args *args = data;

	while (!atomic_load_explicit(args->start, memory_order_acquire))
		sched_yield();
	memset(args->base + args->slice * PROCESS_PAGE, 0x31 + args->slice,
	       PROCESS_PAGE);
	return NULL;
}

static bool concurrent_zero_tuple_promotion(unsigned char *base)
{
	struct concurrent_write_args args[SLICES];
	pthread_t threads[SLICES];
	atomic_bool start = false;
	unsigned char value;
	unsigned int created = 0;
	bool passed = true;

	value = base[2 * PROCESS_PAGE];
	if (value)
		return false;

	for (created = 0; created < SLICES; created++) {
		args[created].base = base;
		args[created].slice = created;
		args[created].start = &start;
		if (pthread_create(&threads[created], NULL,
				   write_zero_tuple_slice, &args[created])) {
			passed = false;
			break;
		}
	}
	atomic_store_explicit(&start, true, memory_order_release);
	while (created)
		passed &= !pthread_join(threads[--created], NULL);

	return passed && verify(base);
}

static int run_test(void)
{
	unsigned char *reservation;
	unsigned char *base;
	uint64_t pfn[SLICES];
	unsigned long rss = 0;
	unsigned long rss_before = 0;
	unsigned long rss_after = 0;
	unsigned long swap = 0;
	bool have_pfns;
	bool passed;
	unsigned char value = 1;

	ksft_print_header();
	ksft_set_plan(16);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE,
			 "process uses 4K pages\n");

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	ksft_test_result(base != MAP_FAILED, "map anonymous test range\n");
	if (base == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	populate(base);
	if (madvise(base, NATIVE_PAGE, MADV_DONTNEED) ||
	    !range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss_before))
		rss_before = ~0UL;
	populate(base);
	if (!range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss_after))
		rss_after = 0;
	have_pfns = read_pfns(base, pfn);
	ksft_test_result(have_pfns && same_pfn(pfn),
			 "four adjacent PTEs map one native folio\n");
	ksft_test_result(rss_before != ~0UL && rss_after >= rss_before &&
			 rss_after - rss_before == NATIVE_PAGE,
			 "packed tuple accounts one native RSS page (%lu bytes)\n",
			 rss_after - rss_before);

	ksft_test_result(fork_cow_preserves_parent(base),
			 "fork COW preserves all four slices\n");

	passed = !mprotect(base + PROCESS_PAGE, PROCESS_PAGE, PROT_READ) &&
		 read_pfns(base, pfn) && distinct_pfns(pfn) && verify(base) &&
		 !mprotect(base + PROCESS_PAGE, PROCESS_PAGE,
			   PROT_READ | PROT_WRITE);
	ksft_test_result(passed,
			 "partial mprotect depacks to singleton folios\n");
	munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && !mlock(base, NATIVE_PAGE) &&
		 read_pfns(base, pfn) && distinct_pfns(pfn) && verify(base) &&
		 !munlock(base, NATIVE_PAGE);
	ksft_test_result(passed, "mlock depacks before setting VM_LOCKED\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	passed = base != MAP_FAILED && concurrent_zero_tuple_promotion(base) &&
		 read_pfns(base, pfn) && same_pfn(pfn);
	ksft_test_result(passed,
			 "concurrent writes promote one zero tuple without splitting\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && !madvise(base, NATIVE_PAGE,
						  MADV_PAGEOUT) &&
		 same_swap_entry(base) &&
		 range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss) && !rss &&
		 range_stat_bytes(base, NATIVE_PAGE, "Swap", &swap) &&
		 swap == NATIVE_PAGE &&
		 verify(base) && read_pfns(base, pfn) && same_pfn(pfn) &&
		 range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss) &&
		 rss == NATIVE_PAGE;
	ksft_test_result(passed,
			 "pageout uses one slot then restores one packed folio\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED &&
		 !madvise(base + PROCESS_PAGE, PROCESS_PAGE, MADV_DONTNEED) &&
		 base[0] == 0x31 && base[PROCESS_PAGE] == 0 &&
		 base[2 * PROCESS_PAGE] == 0x33 &&
		 base[3 * PROCESS_PAGE] == 0x34;
	ksft_test_result(passed,
			 "partial MADV_DONTNEED affects only requested slice\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		value = base[2 * PROCESS_PAGE];
	passed = base != MAP_FAILED && !value && read_pfns(base, pfn) &&
		 same_pfn(pfn);
	ksft_test_result(passed,
			 "one read fault maps a shared four-PTE zero tuple\n");
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && verify(base) && read_pfns(base, pfn) &&
		 same_pfn(pfn);
	ksft_test_result(passed,
			 "first write promotes the zero tuple to one packed folio\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED &&
		 !madvise(base, NATIVE_PAGE, MADV_FREE) && verify(base) &&
		 read_pfns(base, pfn) && same_pfn(pfn) &&
		 range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss) &&
		 rss == NATIVE_PAGE;
	ksft_test_result(passed,
			 "full MADV_FREE preserves a complete packed tuple\n");
	passed = passed && !madvise(base, NATIVE_PAGE, MADV_PAGEOUT) &&
		 all_zero(base) &&
		 range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss) && !rss;
	ksft_test_result(passed,
			 "reclaim discards a clean lazy-free packed tuple\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(DISCARD_GROUPS * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		for (unsigned int i = 0; i < DISCARD_GROUPS; i++)
			populate(base + i * NATIVE_PAGE);
	passed = base != MAP_FAILED &&
		 !madvise(base, DISCARD_GROUPS * NATIVE_PAGE, MADV_PAGEOUT) &&
		 all_groups_swapped(base, DISCARD_GROUPS) &&
		 !madvise(base, NATIVE_PAGE, MADV_FREE) && all_zero(base) &&
		 verify(base + NATIVE_PAGE);
	ksft_test_result(passed,
			 "MADV_FREE safely drops a complete packed swap tuple\n");
	passed = passed && !munmap(base + 4 * NATIVE_PAGE, NATIVE_PAGE) &&
		 verify(base + 5 * NATIVE_PAGE);
	ksft_test_result(passed,
			 "munmap safely drops a complete packed swap tuple\n");
	if (base != MAP_FAILED)
		munmap(reservation, (DISCARD_GROUPS + 1) * NATIVE_PAGE);

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
	execl("/proc/self/exe", "packed_anon_ppps", "--run", NULL);
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
