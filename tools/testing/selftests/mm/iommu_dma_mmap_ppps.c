// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
#define BUFFER_SIZE (4 * USER_PAGE_SIZE)

static int run_test(void)
{
	unsigned char *mapping;
	bool contents_ok = true;
	size_t offset;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/iommu_dma_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open IOMMU DMA test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a complete noncontiguous DMA buffer\n");
	if (mapping == MAP_FAILED) {
		ksft_test_result_skip("DMA buffer mapping is unavailable\n");
		close(fd);
		ksft_finished();
	}

	for (offset = 0; offset < BUFFER_SIZE; offset += USER_PAGE_SIZE) {
		if (mapping[offset] != 0x51 + offset / USER_PAGE_SIZE) {
			contents_ok = false;
			break;
		}
	}
	ksft_test_result(contents_ok,
			 "preserve all four process-page DMA slices\n");

	munmap(mapping, BUFFER_SIZE);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "iommu_dma_mmap_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
