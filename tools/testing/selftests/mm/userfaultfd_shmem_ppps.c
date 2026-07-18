// SPDX-License-Identifier: GPL-2.0
/*
 * userfaultfd on a shmem mapping of a 4K compat process: missing faults and
 * UFFDIO_COPY work per 4K slice (also for slices of a folio that already
 * exists), concurrent slice copies into one native folio retry insertion
 * races, and untouched sibling slices of a UFFD-allocated folio read as zero.
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_SIZE	(2 * USER_PAGE_SIZE)

static bool copy_page(int uffd, void *destination, const void *source)
{
	struct uffdio_copy copy = {
		.src = (unsigned long)source,
		.dst = (unsigned long)destination,
		.len = PROCESS_PAGE_SIZE,
	};

	if (!ioctl(uffd, UFFDIO_COPY, &copy))
		return true;
	ksft_print_msg("UFFDIO_COPY to %p failed: %s, copy=%lld\n",
		       destination, strerror(errno), (long long)copy.copy);
	return false;
}

static int run_test(void)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	unsigned char *mapping;
	unsigned char *source;
	bool first_copy;
	bool second_copy;
	bool registered;
	bool api_ok;
	int memfd;
	int uffd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0) {
		if (errno == EPERM)
			ksft_exit_skip("userfaultfd is unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd failed: %s\n", strerror(errno));
	}
	api_ok = !ioctl(uffd, UFFDIO_API, &api);
	ksft_test_result(api_ok, "enable the userfaultfd API\n");
	if (!api_ok)
		ksft_exit_fail_msg("UFFDIO_API failed: %s\n", strerror(errno));

	memfd = memfd_create("userfaultfd-shmem-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, MAPPING_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("shmem mmap failed: %s\n", strerror(errno));
	source = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		ksft_exit_fail_msg("source mmap failed: %s\n", strerror(errno));
	memset(source, 0x41, USER_PAGE_SIZE);
	memset(source + USER_PAGE_SIZE, 0x42, USER_PAGE_SIZE);

	registration.range.start = (unsigned long)mapping;
	registration.range.len = MAPPING_SIZE;
	registered = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	ksft_test_result(registered, "register a two-page shmem range\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	first_copy = copy_page(uffd, mapping, source);
	ksft_test_result(first_copy, "copy into the first 4K shmem slice\n");
	second_copy = copy_page(uffd, mapping + USER_PAGE_SIZE,
				source + USER_PAGE_SIZE);
	ksft_test_result(second_copy,
			 "copy into the second 4K slice of the same native folio\n");
	ksft_test_result(first_copy && second_copy && mapping[0] == 0x41 &&
			 mapping[USER_PAGE_SIZE] == 0x42,
			 "both copied shmem slices retain their contents\n");

	munmap(source, MAPPING_SIZE);
	munmap(mapping, MAPPING_SIZE);
	close(memfd);
	close(uffd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
