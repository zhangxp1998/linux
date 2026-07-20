// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
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

static int run_test(void)
{
	unsigned long kernel_size, mmu_size;
	unsigned char *mapping;
	bool parsed;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED, "create an anonymous mapping\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	*mapping = 0x5a;
	ksft_test_result(*mapping == 0x5a, "fault the mapping\n");

	parsed = read_page_sizes((uintptr_t)mapping, &kernel_size, &mmu_size);
	ksft_print_msg("KernelPageSize=%lu MMUPageSize=%lu\n",
		       kernel_size, mmu_size);
	ksft_test_result(parsed && kernel_size >= USER_PAGE_SIZE,
			 "read the VMA page sizes from smaps\n");
	ksft_test_result(parsed && mmu_size == USER_PAGE_SIZE,
			 "MMUPageSize matches the process page size\n");

	munmap(mapping, USER_PAGE_SIZE);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("could not enable 4K compatibility mode\n");
	execl("/proc/self/exe", "smaps_page_size_ppps", "--compat", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return run_test();
	return EXIT_FAILURE;
}
