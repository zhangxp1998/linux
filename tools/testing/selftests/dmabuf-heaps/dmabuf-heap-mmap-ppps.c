// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include <linux/dma-heap.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define HEAP_PATH	"/dev/dma_heap/system"
#define USER_PAGE_SIZE	4096
#define NATIVE_PAGE_SIZE	16384
#define BUFFER_SIZE	(2 * NATIVE_PAGE_SIZE)
#define NR_SLICES	(BUFFER_SIZE / USER_PAGE_SIZE)

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "dmabuf-heap-mmap-ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

static int run_test(void)
{
	static const uint32_t values[NR_SLICES] = {
		0x13579bdf, 0x2468ace0, 0x55aa33cc, 0xc001d00d,
		0x10203040, 0x89abcdef, 0x5a5aa5a5, 0xdeadbeef,
	};
	struct dma_heap_allocation_data alloc = {
		.len = BUFFER_SIZE,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	uint8_t *mapping;
	int heap_fd;
	int i;
	int ret;

	ksft_print_header();
	ksft_set_plan(31);

	ksft_test_result(getpagesize() == USER_PAGE_SIZE,
			 "run with a 4K process page size\n");

	heap_fd = open(HEAP_PATH, O_RDWR | O_CLOEXEC);
	ksft_test_result(heap_fd >= 0, "open the system dma-buf heap\n");
	if (heap_fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", HEAP_PATH,
				   strerror(errno));

	ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
	ksft_test_result(ret == 0,
			 "allocate a native-page-sized dma-buf\n");
	if (ret)
		ksft_exit_fail_msg("DMA_HEAP_IOCTL_ALLOC failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       alloc.fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "map the complete dma-buf\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("full dma-buf mmap failed: %s\n",
				   strerror(errno));

	for (i = 0; i < NR_SLICES; i++) {
		uint32_t *marker = (uint32_t *)(mapping +
					       i * USER_PAGE_SIZE);

		*marker = values[i];
		ksft_test_result(*marker == values[i],
				 "write marker to 4K slice %d\n", i);
	}
	munmap(mapping, BUFFER_SIZE);

	for (i = 0; i < NR_SLICES; i++) {
		uint32_t *slice;

		slice = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, alloc.fd, i * USER_PAGE_SIZE);
		ksft_test_result(slice != MAP_FAILED,
				 "map 4K slice %d at a non-native offset\n", i);
		ksft_test_result(slice != MAP_FAILED && *slice == values[i],
				 "read the correct marker from 4K slice %d\n", i);
		if (slice != MAP_FAILED)
			munmap(slice, USER_PAGE_SIZE);
	}

	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, alloc.fd, USER_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map 16K starting at the second 4K slice\n");
	ksft_test_result(mapping != MAP_FAILED &&
			 *(uint32_t *)mapping == values[1],
			 "preserve the leading non-native slice offset\n");
	ksft_test_result(mapping != MAP_FAILED &&
			 *(uint32_t *)(mapping + 3 * USER_PAGE_SIZE) == values[4],
			 "map across the native-page boundary\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, NATIVE_PAGE_SIZE);

	close(alloc.fd);
	close(heap_fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
