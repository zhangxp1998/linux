// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/kernel-page-flags.h>
#include <stdbool.h>
#include <stdint.h>
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

#define USER_PAGE_SIZE 4096UL
#define MAPPING_OFFSET ((1024 - 1) * USER_PAGE_SIZE)
#define MAPPING_SIZE (2 * USER_PAGE_SIZE)
#define WRITE_OFFSET USER_PAGE_SIZE
#define FILE_SIZE (2048 * USER_PAGE_SIZE)
#define TEST_VALUE 0x7b
#define BIT_ULL(nr) (1ULL << (nr))
#define PM_PFRAME_MASK (BIT_ULL(55) - 1)
#define PM_PRESENT BIT_ULL(63)

static bool mapped_pfn(const void *address, uint64_t *pfn)
{
	uint64_t entry;
	off_t offset;
	int fd;
	bool valid;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	offset = ((uintptr_t)address / USER_PAGE_SIZE) * sizeof(entry);
	valid = pread(fd, &entry, sizeof(entry), offset) ==
		(ssize_t)sizeof(entry) &&
		(entry & PM_PRESENT) && (entry & PM_PFRAME_MASK);
	close(fd);
	if (valid)
		*pfn = entry & PM_PFRAME_MASK;
	return valid;
}

static bool pfn_is_dirty(uint64_t pfn, bool *dirty)
{
	uint64_t flags;
	off_t offset;
	int fd;
	bool valid;

	fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	offset = pfn * sizeof(flags);
	valid = pread(fd, &flags, sizeof(flags), offset) ==
		(ssize_t)sizeof(flags);
	close(fd);
	if (valid)
		*dirty = flags & BIT_ULL(KPF_DIRTY);
	return valid;
}

static int run_test(const char *cached_dir)
{
	char cached_path[PATH_MAX];
	unsigned char *mapping;
	unsigned char zero = 0;
	uint64_t pfn;
	bool dirty;
	bool resolved;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (snprintf(cached_path, sizeof(cached_path), "%s/mmap-close.bin",
		     cached_dir) >= (int)sizeof(cached_path))
		ksft_exit_fail_msg("9p test path is too long\n");

	fd = open(cached_path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || ftruncate(fd, FILE_SIZE) ||
	    pwrite(fd, &zero, 1, MAPPING_OFFSET + WRITE_OFFSET) != 1 ||
	    fsync(fd))
		ksft_exit_fail_msg("initialize 9p file failed: %s\n",
				   strerror(errno));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, MAPPING_OFFSET);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a range crossing a native folio boundary\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("9p mmap failed: %s\n", strerror(errno));

	mapping[WRITE_OFFSET] = TEST_VALUE;
	resolved = mapped_pfn(mapping + WRITE_OFFSET, &pfn);
	ksft_test_result(resolved,
			 "resolve the trailing folio through pagemap\n");
	if (!resolved)
		ksft_exit_fail_msg("pagemap PFN is unavailable\n");
	munmap(mapping, MAPPING_SIZE);
	if (!pfn_is_dirty(pfn, &dirty))
		ksft_exit_fail_msg("kpageflags is unavailable: %s\n",
				   strerror(errno));
	ksft_test_result(!dirty,
			 "VMA close writes back the trailing native folio\n");
	close(fd);
	unlink(cached_path);
	ksft_finished();
}

static int exec_compat(const char *cached_dir)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "v9fs_mmap_close_ppps", "--run", cached_dir,
	      NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 2)
		return exec_compat(argv[1]);
	if (argc == 3 && !strcmp(argv[1], "--run"))
		return run_test(argv[2]);
	return EXIT_FAILURE;
}
