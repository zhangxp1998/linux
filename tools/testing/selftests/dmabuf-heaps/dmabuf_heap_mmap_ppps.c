// SPDX-License-Identifier: GPL-2.0
/*
 * A two-native-page system dma-heap buffer maps into a 4K compat process as a
 * whole, one 4K slice at a time at non-native offsets, and as a 16K window
 * starting at the second slice, with every slice's marker preserved.
 */

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/dma-heap.h>

#include "kselftest_ppps.h"

#define HEAP_PATH	"/dev/dma_heap/system"
#define BUFFER_SIZE	(2 * NATIVE_PAGE_SIZE)
#define NR_SLICES	(BUFFER_SIZE / PROCESS_PAGE_SIZE)

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
	ksft_set_plan(30);

	heap_fd = ppps_open_fixture_or_skip(HEAP_PATH, O_RDWR);
	ksft_test_result(heap_fd >= 0, "open the system dma-buf heap\n");

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
					       i * PROCESS_PAGE_SIZE);

		*marker = values[i];
		ksft_test_result(*marker == values[i],
				 "write marker to 4K slice %d\n", i);
	}
	munmap(mapping, BUFFER_SIZE);

	for (i = 0; i < NR_SLICES; i++) {
		uint32_t *slice;

		slice = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, alloc.fd, i * PROCESS_PAGE_SIZE);
		ksft_test_result(slice != MAP_FAILED,
				 "map 4K slice %d at a non-native offset\n", i);
		ksft_test_result(slice != MAP_FAILED && *slice == values[i],
				 "read the correct marker from 4K slice %d\n", i);
		if (slice != MAP_FAILED)
			munmap(slice, PROCESS_PAGE_SIZE);
	}

	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, alloc.fd, PROCESS_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map 16K starting at the second 4K slice\n");
	ksft_test_result(mapping != MAP_FAILED &&
			 *(uint32_t *)mapping == values[1],
			 "preserve the leading non-native slice offset\n");
	ksft_test_result(mapping != MAP_FAILED &&
			 *(uint32_t *)(mapping + 3 * PROCESS_PAGE_SIZE) == values[4],
			 "map across the native-page boundary\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, NATIVE_PAGE_SIZE);

	close(alloc.fd);
	close(heap_fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
