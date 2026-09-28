// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_WILLNEED on a two-4K-page file range that straddles two native pages
 * is accepted by a 4K compat process and brings both native pages back into
 * the page cache after eviction.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "p3s_harness.h"

#define MAPPING_OFFSET	(3 * PROCESS_PAGE_SIZE)
#define MAPPING_LENGTH	(2 * PROCESS_PAGE_SIZE)
#define FILE_SIZE	(64 * PROCESS_PAGE_SIZE)
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
		contents[i] = 0x31 + (i / PROCESS_PAGE_SIZE) % 31;
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

TEST(willneed_refaults_both_native_pages_of_4k_range)
{
	char path[] = "madvise-willneed-p3s.XXXXXX";
	void *mapping = MAP_FAILED;
	int fd = -1;
	int ret;

	fd = mkostemp(path, O_CLOEXEC);
	EXPECT_GE(fd, 0);
	if (fd < 0)
		return;
	ASSERT_EQ(0, unlink(path));
	if (!initialize_file(fd)) {
		EXPECT_TRUE(false);
		goto out;
	}
	mapping = mmap(NULL, MAPPING_LENGTH, PROT_READ, MAP_SHARED, fd,
		       MAPPING_OFFSET);
	EXPECT_NE(mapping, MAP_FAILED);
	if (mapping == MAP_FAILED)
		goto out;

	if (!evict_mapping(fd, mapping)) {
		munmap(mapping, MAPPING_LENGTH);
		mapping = MAP_FAILED;
		close(fd);
		fd = -1;
		SKIP(return, "backing filesystem could not evict test pages");
	}

	errno = 0;
	ret = madvise(mapping, MAPPING_LENGTH, MADV_WILLNEED);
	EXPECT_EQ(0, ret);
	if (!ret)
		EXPECT_TRUE(wait_for_willneed(mapping));

out:
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_LENGTH);
	if (fd >= 0)
		close(fd);
}

P3S_HARNESS_4KB_MAIN
