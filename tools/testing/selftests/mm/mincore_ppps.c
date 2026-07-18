// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kselftest.h"

#define USER_PAGE_SIZE	4096UL
#define LARGE_PAGE_SIZE	16384UL
#define MAPPING_SIZE	(12 * USER_PAGE_SIZE)
#define SENTINEL	0xa5

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool entries_are_resident(const unsigned char *vec, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (vec[i] == SENTINEL || !(vec[i] & 1))
			return false;
	}
	return vec[nr] == SENTINEL;
}

static bool test_4k_only_alignment(unsigned char *large_page_aligned)
{
	unsigned char vec = SENTINEL;
	void *start = large_page_aligned + USER_PAGE_SIZE;

	errno = 0;
	if (mincore(start, USER_PAGE_SIZE, &vec)) {
		ksft_print_msg("mincore(4K-only aligned) failed: %s\n",
			       strerror(errno));
		return false;
	}
	return vec & 1;
}

static bool test_vector_granularity(unsigned char *large_page_aligned)
{
	unsigned char vec[8];

	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(large_page_aligned, LARGE_PAGE_SIZE, vec)) {
		ksft_print_msg("mincore(16K) failed: %s\n", strerror(errno));
		return false;
	}
	ksft_print_msg("16K vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	return entries_are_resident(vec, 4);
}

static bool test_partial_length_rounding(unsigned char *large_page_aligned)
{
	const size_t length = 3 * USER_PAGE_SIZE + 1;
	unsigned char vec[8];

	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(large_page_aligned, length, vec)) {
		ksft_print_msg("mincore(partial length) failed: %s\n",
			       strerror(errno));
		return false;
	}
	ksft_print_msg("partial vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	return entries_are_resident(vec, 4);
}

static bool test_file_page_cache_index(void)
{
	unsigned char vec[8];
	unsigned char *contents = NULL;
	unsigned char *mapping = MAP_FAILED;
	bool passed = false;
	ssize_t written;
	int fd = -1;

	fd = memfd_create("mincore-ppps", MFD_CLOEXEC);
	if (fd < 0) {
		ksft_print_msg("memfd_create failed: %s\n", strerror(errno));
		goto out;
	}
	contents = malloc(LARGE_PAGE_SIZE);
	if (!contents) {
		ksft_print_msg("malloc failed\n");
		goto out;
	}
	memset(contents, 0x6d, LARGE_PAGE_SIZE);
	written = pwrite(fd, contents, LARGE_PAGE_SIZE, 0);
	if (written != (ssize_t)LARGE_PAGE_SIZE) {
		ksft_print_msg("pwrite returned %zd: %s\n", written,
			       strerror(errno));
		goto out;
	}

	/* Leave the PTEs absent so mincore() has to query the page cache. */
	mapping = mmap(NULL, LARGE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		ksft_print_msg("file mmap failed: %s\n", strerror(errno));
		goto out;
	}
	memset(vec, SENTINEL, sizeof(vec));
	if (mincore(mapping, LARGE_PAGE_SIZE, vec)) {
		ksft_print_msg("file mincore failed: %s\n", strerror(errno));
		goto out;
	}
	ksft_print_msg("file vector: %02x,%02x,%02x,%02x next=%02x\n",
		       vec[0], vec[1], vec[2], vec[3], vec[4]);
	passed = entries_are_resident(vec, 4);

out:
	if (mapping != MAP_FAILED)
		munmap(mapping, LARGE_PAGE_SIZE);
	free(contents);
	if (fd >= 0)
		close(fd);
	return passed;
}

int main(void)
{
	unsigned char *large_page_aligned;
	unsigned char *mapping;
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned int i;

	ksft_print_header();
	if (page_size != USER_PAGE_SIZE)
		ksft_exit_skip("requires a 4K userspace page size\n");
	ksft_set_plan(4);

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap failed: %s\n", strerror(errno));
	large_page_aligned = (unsigned char *)align_up((uintptr_t)mapping,
							LARGE_PAGE_SIZE);
	if (large_page_aligned + LARGE_PAGE_SIZE > mapping + MAPPING_SIZE)
		ksft_exit_fail_msg("aligned range exceeds mapping\n");

	for (i = 0; i < LARGE_PAGE_SIZE / USER_PAGE_SIZE; i++)
		large_page_aligned[i * USER_PAGE_SIZE] = (unsigned char)(0x40 + i);

	ksft_test_result(test_4k_only_alignment(large_page_aligned),
			 "mincore accepts a 4K-only-aligned address\n");
	ksft_test_result(test_vector_granularity(large_page_aligned),
			 "mincore returns one byte per userspace 4K page\n");
	ksft_test_result(test_partial_length_rounding(large_page_aligned),
			 "mincore rounds partial lengths at 4K granularity\n");
	ksft_test_result(test_file_page_cache_index(),
			 "file-backed 4K slices use the correct page-cache index\n");

	munmap(mapping, MAPPING_SIZE);
	ksft_finished();
}
