// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SLICE_SIZE 4096UL
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

int main(int argc, char **argv)
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
