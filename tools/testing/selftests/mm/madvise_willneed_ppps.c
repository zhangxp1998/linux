// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kselftest.h"

#define USER_PAGE_SIZE	4096UL
#define MAPPING_OFFSET	(3 * USER_PAGE_SIZE)
#define MAPPING_LENGTH	(2 * USER_PAGE_SIZE)
#define FILE_SIZE	(64 * USER_PAGE_SIZE)
#define POLL_ATTEMPTS	100

static bool initialize_file(int fd)
{
	unsigned char *contents;
	ssize_t written;
	size_t i;
	bool passed = false;

	contents = malloc(FILE_SIZE);
	if (!contents)
		return false;
	for (i = 0; i < FILE_SIZE; i++)
		contents[i] = 0x31 + (i / USER_PAGE_SIZE) % 31;
	written = pwrite(fd, contents, FILE_SIZE, 0);
	if (written != (ssize_t)FILE_SIZE) {
		ksft_print_msg("pwrite returned %zd: %s\n", written,
			       strerror(errno));
		goto out;
	}
	if (fsync(fd)) {
		ksft_print_msg("fsync failed: %s\n", strerror(errno));
		goto out;
	}
	passed = true;

out:
	free(contents);
	return passed;
}

static bool get_residency(void *mapping, unsigned char vec[2])
{
	memset(vec, 0xff, 2);
	if (mincore(mapping, MAPPING_LENGTH, vec)) {
		ksft_print_msg("mincore failed: %s\n", strerror(errno));
		return false;
	}
	vec[0] &= 1;
	vec[1] &= 1;
	return true;
}

static bool evict_mapping(int fd, void *mapping)
{
	unsigned char vec[2];
	unsigned int attempt;

	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		int error = posix_fadvise(fd, 0, FILE_SIZE, POSIX_FADV_DONTNEED);

		if (error) {
			ksft_print_msg("POSIX_FADV_DONTNEED failed: %s\n",
				       strerror(error));
			return false;
		}
		if (!get_residency(mapping, vec))
			return false;
		if (!vec[0] && !vec[1])
			return true;
		usleep(10000);
	}
	ksft_print_msg("could not evict mapped range: vec=%u,%u\n",
		       vec[0], vec[1]);
	return false;
}

static bool wait_for_willneed(void *mapping)
{
	unsigned char vec[2];
	unsigned int attempt;

	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!get_residency(mapping, vec))
			return false;
		if (vec[0] && vec[1])
			return true;
		usleep(10000);
	}
	ksft_print_msg("post-MADV_WILLNEED residency: vec=%u,%u\n",
		       vec[0], vec[1]);
	return false;
}

int main(int argc, char **argv)
{
	const char *path;
	void *mapping = MAP_FAILED;
	bool evicted;
	int fd = -1;
	int ret;

	ksft_print_header();
	if (sysconf(_SC_PAGESIZE) != USER_PAGE_SIZE)
		ksft_exit_skip("requires a 4K userspace page size\n");
	if (argc != 2)
		ksft_exit_fail_msg("usage: %s FILE\n", argv[0]);
	path = argv[1];

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		ksft_exit_fail_msg("open(%s) failed: %s\n", path,
				   strerror(errno));
	if (!initialize_file(fd))
		ksft_exit_fail_msg("failed to initialize backing file\n");
	mapping = mmap(NULL, MAPPING_LENGTH, PROT_READ, MAP_SHARED, fd,
		       MAPPING_OFFSET);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	evicted = evict_mapping(fd, mapping);
	if (!evicted)
		ksft_exit_skip("backing filesystem could not evict test pages\n");
	ksft_set_plan(2);

	errno = 0;
	ret = madvise(mapping, MAPPING_LENGTH, MADV_WILLNEED);
	if (ret)
		ksft_print_msg("MADV_WILLNEED failed: %s\n", strerror(errno));
	ksft_test_result(!ret, "MADV_WILLNEED accepts the 4K range\n");
	ksft_test_result(!ret && wait_for_willneed(mapping),
			 "MADV_WILLNEED reads both native pages in the range\n");

	munmap(mapping, MAPPING_LENGTH);
	close(fd);
	unlink(path);
	ksft_finished();
}
