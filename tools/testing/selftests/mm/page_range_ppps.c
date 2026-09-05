// SPDX-License-Identifier: GPL-2.0
/* Exact byte-range GUP results, including short success and duplicate pins. */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kselftest_ppps.h"
#include "iov_iter_ppps.h"
#include "page_range_ppps.h"

static void check_range(int fd, unsigned char *base, size_t page_size,
			size_t backing_offset, const char *name, bool pin)
{
	struct page_range_ppps_args req = {
		.address = (uintptr_t)base + 17,
		.length = page_size,
		.capacity = PAGE_RANGE_PPPS_MAX,
		.pin = pin,
	};
	int ret = ioctl(fd, PAGE_RANGE_PPPS_IOCTL, &req);

	ksft_test_result(
		!ret && req.helpers_ok && req.result == 2 &&
			req.spans[0].offset == backing_offset + 17 &&
			req.spans[0].length == page_size - 17 &&
			req.spans[0].first == 0x31 &&
			req.spans[0].last == 0x31 &&
			req.spans[1].offset == (backing_offset + page_size) %
						       NATIVE_PAGE_SIZE &&
			req.spans[1].length == 17 &&
			req.spans[1].first == 0x72 && req.spans[1].last == 0x72,
		"%s %s exact byte spans and helper units\n",
		pin ? "pin" : "get", name);
}

static int run_range_test(void)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	struct page_range_ppps_args req = {};
	unsigned char *anon, *file;
	int fd, memfd, pin;

	ksft_print_header();
	ksft_set_plan(9);
	fd = ppps_open_fixture_or_skip("/dev/" IOV_ITER_PPPS_DEVICE_NAME,
				       O_RDWR);
	memfd = memfd_create("page-range", 0);
	if (memfd < 0 || ftruncate(memfd, 3 * NATIVE_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed\n");
	anon = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	file = mmap(NULL, 2 * NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, memfd, page_size);
	if (anon == MAP_FAILED || file == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed\n");
	memset(anon, 0x31, page_size);
	memset(anon + page_size, 0x72, page_size);
	memset(file, 0x31, page_size);
	memset(file + page_size, 0x72, page_size);
	for (pin = 0; pin <= 1; pin++) {
		check_range(fd, anon, page_size, 0, "anonymous", pin);
		check_range(fd, file, page_size, page_size % NATIVE_PAGE_SIZE,
			    "file with nonzero offset", pin);
	}
	req.address = (uintptr_t)anon + 17;
	req.length = page_size;
	req.capacity = 1;
	req.pin = 1;
	ksft_test_result(!ioctl(fd, PAGE_RANGE_PPPS_IOCTL, &req) &&
				 req.result == -ENOSPC,
			 "insufficient capacity owns no pins\n");
	req.length = 0;
	req.capacity = 0;
	ksft_test_result(!ioctl(fd, PAGE_RANGE_PPPS_IOCTL, &req) &&
				 req.result == 0,
			 "empty byte range needs no output slots\n");
	req.address = UINT64_MAX - 31;
	req.length = 64;
	req.capacity = 2;
	/* Tag stripping may turn the wrap into an inaccessible address. */
	ksft_test_result(!ioctl(fd, PAGE_RANGE_PPPS_IOCTL, &req) &&
				 req.result < 0,
			 "invalid high byte range is rejected\n");
	if (mprotect(anon + page_size, page_size, PROT_NONE))
		ksft_exit_fail_msg("mprotect failed\n");
	for (pin = 0; pin <= 1; pin++) {
		req.address = (uintptr_t)anon + 17;
		req.length = page_size;
		req.capacity = 2;
		req.pin = pin;
		ksft_test_result(
			!ioctl(fd, PAGE_RANGE_PPPS_IOCTL, &req) &&
				req.result == 1 &&
				req.spans[0].length == page_size - 17 &&
				req.spans[0].offset == 17,
			"%s short result describes only accessible bytes\n",
			pin ? "pin" : "get");
	}
	munmap(anon, 3 * NATIVE_PAGE_SIZE);
	munmap(file, 2 * NATIVE_PAGE_SIZE);
	close(memfd);
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (sysconf(_SC_PAGESIZE) != NATIVE_PAGE_SIZE)
			exec_native(argv[0], "--native", NULL);
		return run_range_test();
	}
	return ppps_compat_main(argc, argv, run_range_test);
}
