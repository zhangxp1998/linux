// SPDX-License-Identifier: GPL-2.0
/* Check that pagemap's exclusive bit is evaluated per PPPS file slice. */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#define NR_SLICES 4
#define MAPPING_SIZE (NR_SLICES * PROCESS_PAGE_SIZE)

static int failures;
static int test_no;

static void result(bool pass, const char *name)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++test_no, name);
	if (!pass)
		failures++;
}

static bool check_exclusive(unsigned char *mapping, unsigned int exclusive_mask)
{
	unsigned int observed = 0;
	unsigned int i;

	for (i = 0; i < NR_SLICES; i++) {
		uint64_t entry;

		if (!ppps_pagemap_entry(mapping + i * PROCESS_PAGE_SIZE, &entry)) {
			perror("read pagemap");
			return false;
		}
		if ((entry & (PAGEMAP_PRESENT | PAGEMAP_FILE_SHARED)) !=
		    (PAGEMAP_PRESENT | PAGEMAP_FILE_SHARED)) {
			printf("# slice %u missing present/file flags: %#llx\n", i,
			       (unsigned long long)entry);
			return false;
		}
		if (entry & PAGEMAP_EXCLUSIVE)
			observed |= 1U << i;
	}
	printf("# exclusive mask: expected=%#x observed=%#x\n",
	       exclusive_mask, observed);
	return observed == exclusive_mask;
}

static int run_test(void)
{
	unsigned char *mapping = MAP_FAILED;
	unsigned char *alias = MAP_FAILED;
	unsigned int i;
	uint64_t alias_entry;
	int fd = -1;

	printf("TAP version 13\n1..4\n");

	fd = syscall(SYS_memfd_create, "pagemap-exclusive-ppps", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, MAPPING_SIZE)) {
		perror("prepare memfd");
		goto out;
	}
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap target");
		goto out;
	}
	for (i = 0; i < NR_SLICES; i++)
		mapping[i * PROCESS_PAGE_SIZE] = (unsigned char)i;
	result(check_exclusive(mapping, 0xf),
	       "disjoint file slices are exclusively mapped");

	alias = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	if (alias == MAP_FAILED) {
		perror("mmap alias");
		goto out;
	}
	alias[0] ^= 1;
	result(check_exclusive(mapping, 0xe),
	       "only the aliased slice loses exclusivity");

	if (!ppps_pagemap_entry(alias, &alias_entry)) {
		perror("read alias pagemap");
		goto out;
	}
	result(!(alias_entry & PAGEMAP_EXCLUSIVE),
	       "the second mapping of a slice is not exclusive");

	munmap(alias, PROCESS_PAGE_SIZE);
	alias = MAP_FAILED;
	result(check_exclusive(mapping, 0xf),
	       "unmapping the alias restores exclusivity");

out:
	if (test_no < 4)
		failures += 4 - test_no;
	if (alias != MAP_FAILED)
		munmap(alias, PROCESS_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);
	if (fd >= 0)
		close(fd);
	printf("# Totals: pass:%d fail:%d\n", 4 - failures, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (access("/proc/self", F_OK) &&
	    mount("proc", "/proc", "proc", 0, NULL)) {
		perror("mount proc");
		return EXIT_FAILURE;
	}
	return ppps_compat_main(argc, argv, run_test);
}
