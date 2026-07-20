// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_COLD over a 2MB shared file mapping placed across a 4K compat
 * process's PMD boundary keeps every 4K file slice's contents and leaves
 * the kernel free of a bad-page taint.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "kselftest_ppps.h"

#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)

static int failures;
static int test_no;

static void result(int pass, const char *name)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++test_no, name);
	if (!pass)
		failures++;
}

static void skip(const char *name, const char *reason)
{
	printf("ok %d - %s # SKIP %s\n", ++test_no, name, reason);
}

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

static int run_test(const char *file)
{
	unsigned char checksum = 0;
	unsigned char *map;
	size_t i;
	int fd;
	int rc;

	printf("TAP version 13\n1..4\n");
	if (argc != 2) {
		printf("Bail out! usage: %s FILE\n", argv[0]);
		return 1;
	}

	result(sysconf(_SC_PAGESIZE) == SLICE_SIZE,
	       "process page size is 4K");

	fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	rc = fd < 0 ? -1 : ftruncate(fd, MAP_SIZE);
	if (rc)
		perror("prepare sparse file");
	result(rc == 0, "create a 2MB sparse file");
	if (rc)
		goto out_close;

	map = mmap(MAP_ADDR, MAP_SIZE, PROT_READ,
		   MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		result(0, "fault a file mapping across a process PMD");
		goto out_close;
	}
	for (i = 0; i < MAP_SIZE; i += SLICE_SIZE)
		checksum ^= map[i];
	printf("# checksum before MADV_COLD=%u\n", checksum);
	result(map == MAP_ADDR, "fault a file mapping across a process PMD");

	rc = madvise(map, MAP_SIZE, MADV_COLD);
	if (rc)
		perror("madvise(MADV_COLD)");
	checksum ^= map[MAP_SIZE - SLICE_SIZE];
	printf("# checksum after MADV_COLD=%u\n", checksum);
	result(rc == 0, "MADV_COLD preserves a usable mapping");

	munmap(map, MAP_SIZE);
out_close:
	if (fd >= 0)
		close(fd);
	unlink(argv[1]);
	printf("# Totals: pass:%d fail:%d\n", test_no - failures, failures);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *mode = ppps_run_mode(argc, argv, NULL);

	if (!mode) {
		if (argc != 2) {
			printf("Bail out! usage: %s FILE\n", argv[0]);
			return 1;
		}
		if (!ppps_is_compat_process())
			exec_compat(argv[0], PPPS_RUN_FLAG, argv[1], NULL);
		return run_test(argv[1]);
	}
	if (argc == 3 && !strcmp(mode, PPPS_RUN_FLAG)) {
		ppps_require_compat();
		return run_test(argv[2]);
	}
	return EXIT_FAILURE;
}
