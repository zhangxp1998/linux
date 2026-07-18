// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_SIZE	(2 * USER_PAGE_SIZE)

static bool page_has_value(const unsigned char *page, unsigned char value)
{
	unsigned long i;

	for (i = 0; i < USER_PAGE_SIZE; i++) {
		if (page[i] != value)
			return false;
	}
	return true;
}

static bool copy_page(int uffd, void *destination, const void *source)
{
	struct uffdio_copy copy = {
		.src = (unsigned long)source,
		.dst = (unsigned long)destination,
		.len = USER_PAGE_SIZE,
	};

	if (!ioctl(uffd, UFFDIO_COPY, &copy))
		return true;
	ksft_print_msg("UFFDIO_COPY failed: %s, copy=%lld\n",
		       strerror(errno), (long long)copy.copy);
	return false;
}

static int run_test(void)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_zeropage zeropage = {};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	unsigned char *mapping;
	unsigned char *source;
	bool first_copy;
	bool registered;
	bool zeroed;
	bool api_ok;
	int memfd;
	int uffd;

	ksft_print_header();
	ksft_set_plan(7);
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

	memfd = memfd_create("userfaultfd-zeropage-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, MAPPING_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("shmem mmap failed: %s\n", strerror(errno));
	source = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		ksft_exit_fail_msg("source mmap failed: %s\n", strerror(errno));
	memset(source, 0xa5, USER_PAGE_SIZE);

	registration.range.start = (unsigned long)mapping;
	registration.range.len = MAPPING_SIZE;
	registered = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	ksft_test_result(registered, "register a two-page shmem range\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	first_copy = copy_page(uffd, mapping, source);
	ksft_test_result(first_copy,
			 "copy nonzero data into the first 4K shmem slice\n");
	zeropage.range.start = (unsigned long)mapping + USER_PAGE_SIZE;
	zeropage.range.len = USER_PAGE_SIZE;
	zeroed = !ioctl(uffd, UFFDIO_ZEROPAGE, &zeropage);
	ksft_test_result(zeroed, "zero the second 4K shmem slice\n");
	if (!zeroed)
		ksft_print_msg("UFFDIO_ZEROPAGE failed: %s, zeropage=%lld\n",
			       strerror(errno), (long long)zeropage.zeropage);
	ksft_test_result(first_copy && page_has_value(mapping, 0xa5),
			 "zeroing preserves the first shmem slice\n");
	ksft_test_result(zeroed &&
			 page_has_value(mapping + USER_PAGE_SIZE, 0),
			 "the requested shmem slice is completely zero\n");

	munmap(source, USER_PAGE_SIZE);
	munmap(mapping, MAPPING_SIZE);
	close(memfd);
	close(uffd);
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
	execl("/proc/self/exe", "userfaultfd_zeropage_ppps", "--run", NULL);
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
