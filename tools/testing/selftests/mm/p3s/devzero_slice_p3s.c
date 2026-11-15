// SPDX-License-Identifier: GPL-2.0
/* Private /dev/zero VMAs retain byte-accurate file offsets. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "p3s_harness.h"

struct zero_offsets_result {
	bool split;
	bool merged;
};

static bool zero_range(unsigned char *address, unsigned char *start,
		       unsigned char *end, unsigned long expected_offset)
{
	unsigned long first, last, offset;
	char permissions[5], line[512];
	FILE *maps = fopen("/proc/self/maps", "r");
	bool match = false;

	if (!maps)
		return false;
	while (fgets(line, sizeof(line), maps)) {
		if (sscanf(line, "%lx-%lx %4s %lx", &first, &last,
			   permissions, &offset) != 4 ||
		    (unsigned long)address < first || (unsigned long)address >= last)
			continue;
		match = first == (unsigned long)start && last == (unsigned long)end &&
			offset == expected_offset;
		break;
	}
	fclose(maps);
	return match;
}

static struct zero_offsets_result test_zero_offsets(void)
{
	struct zero_offsets_result result = {};
	int fd = open("/dev/zero", O_RDWR);
	unsigned long len = 2 * NATIVE_PAGE_SIZE;
	unsigned char *map = MAP_FAILED;

	if (fd >= 0)
		map = mmap(NULL, len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE, fd, 2 * PROCESS_PAGE_SIZE);
	if (map != MAP_FAILED) {
		for (unsigned long i = 0; i < len; i += PROCESS_PAGE_SIZE)
			map[i] = (unsigned char)(0x51 + i / PROCESS_PAGE_SIZE);
		result.split = !mprotect(map + PROCESS_PAGE_SIZE, len - PROCESS_PAGE_SIZE,
				  PROT_READ) &&
			zero_range(map + PROCESS_PAGE_SIZE, map + PROCESS_PAGE_SIZE,
				   map + len, 3 * PROCESS_PAGE_SIZE);
		result.merged = !mprotect(map, len, PROT_READ | PROT_WRITE) &&
			zero_range(map, map, map + len, 2 * PROCESS_PAGE_SIZE);
		for (unsigned long i = 0; i < len; i += PROCESS_PAGE_SIZE)
			result.merged &= map[i] == (unsigned char)(0x51 + i / PROCESS_PAGE_SIZE);
		munmap(map, len);
	}
	if (fd >= 0)
		close(fd);
	return result;
}

TEST(devzero_split_keeps_file_offset)
{
	struct zero_offsets_result result = test_zero_offsets();

	EXPECT_TRUE(result.split);
}

TEST(devzero_merge_preserves_slices)
{
	struct zero_offsets_result result = test_zero_offsets();

	EXPECT_TRUE(result.merged);
}

P3S_HARNESS_COMPAT_MAIN
