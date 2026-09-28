// SPDX-License-Identifier: GPL-2.0
/*
 * smaps reports native 16K KernelPageSize/MMUPageSize on a native process and
 * 16K KernelPageSize / 4K MMUPageSize on a 4K compat process. Run the main
 * binary natively so smaps MMUPageSize is not checked circularly by setup.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_p3s.h"
#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

static bool read_page_sizes(uintptr_t address, unsigned long *kernel_size,
			    unsigned long *mmu_size)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	*kernel_size = 0;
	*mmu_size = 0;
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1)
			*kernel_size = size_kb * 1024;
		else if (sscanf(line, "MMUPageSize: %lu kB", &size_kb) == 1)
			*mmu_size = size_kb * 1024;
		if (*kernel_size && *mmu_size)
			break;
	}
	free(line);
	fclose(file);
	return *kernel_size && *mmu_size;
}

static int check_compat_smaps(void)
{
	unsigned long kernel_size = 0, mmu_size = 0;
	int persona = personality(0xffffffffUL);
	unsigned char *mapping;
	bool parsed;

	if (persona < 0 || !(persona & ADDR_PAGE_SIZE_4KB) ||
	    getpagesize() != PROCESS_PAGE_SIZE)
		return KSFT_SKIP;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return KSFT_FAIL;
	*mapping = 0x5a;
	parsed = read_page_sizes((uintptr_t)mapping, &kernel_size, &mmu_size);
	ksft_print_msg("compat KernelPageSize=%lu MMUPageSize=%lu\n",
		       kernel_size, mmu_size);
	munmap(mapping, PROCESS_PAGE_SIZE);
	if (!parsed || kernel_size != NATIVE_PAGE_SIZE ||
	    mmu_size != PROCESS_PAGE_SIZE)
		return KSFT_FAIL;
	return KSFT_PASS;
}

static int child_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--compat-child"))
		return check_compat_smaps();
	return -1;
}

TEST(native_smaps_reports_native_page_sizes)
{
	unsigned long kernel_size = 0, mmu_size = 0;
	unsigned char *mapping;
	bool parsed;

	if (!p3s_has_native_pages())
		SKIP(return, "requires native 16K pages");
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, mapping);
	*mapping = 0x5a;
	EXPECT_EQ(0x5a, *mapping);

	parsed = read_page_sizes((uintptr_t)mapping, &kernel_size, &mmu_size);
	ksft_print_msg("native KernelPageSize=%lu MMUPageSize=%lu\n",
		       kernel_size, mmu_size);
	EXPECT_TRUE(parsed);
	EXPECT_EQ(NATIVE_PAGE_SIZE, kernel_size);
	EXPECT_EQ(NATIVE_PAGE_SIZE, mmu_size);
	munmap(mapping, NATIVE_PAGE_SIZE);
}

TEST(compat_smaps_reports_kernel_and_mmu_page_sizes)
{
	int ret = p3s_run_child(true, "--compat-child", NULL);

	if (ret == KSFT_SKIP)
		SKIP(return, "4K compat process unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

P3S_HARNESS_4KB_MAIN
