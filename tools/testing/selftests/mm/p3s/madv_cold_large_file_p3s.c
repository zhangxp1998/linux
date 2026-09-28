// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_COLD over a 2MB shared file mapping placed across a 4K compat
 * process's PMD boundary keeps every 4K file slice's contents and leaves
 * the kernel free of a bad-page taint.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_harness.h"

#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)
#define TAINT_BAD_PAGE_MASK 32UL

static int read_kernel_taint(unsigned long *taint)
{
	FILE *file;
	int rc;

	file = fopen("/proc/sys/kernel/tainted", "re");
	if (!file)
		return -1;
	rc = fscanf(file, "%lu", taint) == 1 ? 0 : -1;
	fclose(file);
	return rc;
}

static unsigned char slice_pattern(size_t offset)
{
	return ((offset / PROCESS_PAGE_SIZE) % 251) + 1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[PROCESS_PAGE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		memset(buffer, slice_pattern(offset), sizeof(buffer));
		if (pwrite(fd, buffer, sizeof(buffer), offset) !=
		    (ssize_t)sizeof(buffer))
			return -1;
	}
	if (fsync(fd))
		return -1;
	rc = posix_fadvise(fd, 0, MAP_SIZE, POSIX_FADV_DONTNEED);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

static size_t mapping_mismatches(const unsigned char *map,
				 size_t *checksum)
{
	size_t mismatches = 0;
	size_t offset;

	*checksum = 0;
	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		*checksum += map[offset];
		if (map[offset] != slice_pattern(offset))
			mismatches++;
	}
	return mismatches;
}

static const char *file_prefix;

FIXTURE(cold_file) {
	unsigned char *map;
	int fd;
};

FIXTURE_SETUP(cold_file)
{
	char path[PATH_MAX];
	int length;

	length = snprintf(path, sizeof(path), "%s.XXXXXX", file_prefix);
	ASSERT_GT(length, 0);
	ASSERT_LT(length, (int)sizeof(path));
	self->fd = mkostemp(path, O_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, unlink(path));
	ASSERT_EQ(0, prepare_file(self->fd));
	self->map = mmap(MAP_ADDR, MAP_SIZE, PROT_READ,
			 MAP_SHARED | MAP_FIXED_NOREPLACE, self->fd, 0);
	ASSERT_NE(self->map, MAP_FAILED);
}

FIXTURE_TEARDOWN(cold_file)
{
	munmap(self->map, MAP_SIZE);
	close(self->fd);
}

TEST_F(cold_file, file_slices_across_process_pmd)
{
	size_t checksum;

	EXPECT_EQ(self->map, MAP_ADDR);
	EXPECT_EQ(0, mapping_mismatches(self->map, &checksum));
	TH_LOG("file checksum before MADV_COLD=%zu", checksum);
}

TEST_F(cold_file, madvise_preserves_file_slices)
{
	size_t checksum;

	ASSERT_EQ(0, mapping_mismatches(self->map, &checksum));
	ASSERT_EQ(0, madvise(self->map, MAP_SIZE, MADV_COLD));
	EXPECT_EQ(0, mapping_mismatches(self->map, &checksum));
	TH_LOG("file checksum after MADV_COLD=%zu", checksum);
}

TEST_F(cold_file, madvise_does_not_set_bad_page_taint)
{
	unsigned long before, after;

	if (read_kernel_taint(&before))
		SKIP(return, "kernel taint state is unavailable");
	if (before & TAINT_BAD_PAGE_MASK)
		SKIP(return, "kernel was already tainted by a bad page");
	ASSERT_EQ(0, madvise(self->map, MAP_SIZE, MADV_COLD));
	ASSERT_EQ(0, mapping_mismatches(self->map, &(size_t){ 0 }));
	ASSERT_EQ(0, read_kernel_taint(&after));
	EXPECT_EQ(0, after & TAINT_BAD_PAGE_MASK);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode) {
		if (argc != 2) {
			fprintf(stderr, "usage: %s FILE\n", argv[0]);
			return 1;
		}
		if (!p3s_is_compat_process())
			exec_compat(argv[0], P3S_RUN_FLAG, argv[1], NULL);
		file_prefix = argv[1];
		return p3s_harness_run();
	}
	if (argc == 3 && !strcmp(mode, P3S_RUN_FLAG)) {
		p3s_require_compat();
		file_prefix = argv[2];
		return p3s_harness_run();
	}
	return EXIT_FAILURE;
}
