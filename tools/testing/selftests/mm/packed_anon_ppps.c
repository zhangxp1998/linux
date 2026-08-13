// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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

static int run_test(void)
{
	unsigned char *reservation;
	unsigned char *base;
	uint64_t pfn[SLICES];
	bool have_pfns;
	bool passed;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE,
			 "process uses 4K pages\n");

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	ksft_test_result(base != MAP_FAILED, "map anonymous test range\n");
	if (base == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	populate(base);
	have_pfns = read_pfns(base, pfn);
	ksft_test_result(have_pfns && same_pfn(pfn),
			 "four adjacent PTEs map one native folio\n");

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
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && !madvise(base, NATIVE_PAGE,
						  MADV_PAGEOUT) &&
		 same_swap_entry(base) && verify(base) &&
		 read_pfns(base, pfn) && same_pfn(pfn);
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
