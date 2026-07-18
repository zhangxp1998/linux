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

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_SIZE	(2 * USER_PAGE_SIZE)

static int run_test(void)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	struct uffdio_copy copy = {};
	unsigned char *mapping;
	unsigned char *source;
	bool rejected;
	bool registered;
	bool api_ok;
	int memfd;
	int uffd;
	int result;
	int error;

	ksft_print_header();
	ksft_set_plan(4);
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

	memfd = memfd_create("userfaultfd-eof-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, USER_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	source = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || source == MAP_FAILED)
		ksft_exit_fail_msg("mmap setup failed: %s\n", strerror(errno));
	memset(source, 0x45, USER_PAGE_SIZE);

	registration.range.start = (unsigned long)mapping + USER_PAGE_SIZE;
	registration.range.len = USER_PAGE_SIZE;
	registered = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	ksft_test_result(registered, "register a shmem slice beyond EOF\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	copy.src = (unsigned long)source;
	copy.dst = (unsigned long)mapping + USER_PAGE_SIZE;
	copy.len = USER_PAGE_SIZE;
	result = ioctl(uffd, UFFDIO_COPY, &copy);
	error = errno;
	rejected = result == -1 && (error == EFAULT || error == ENOMEM) &&
		   copy.copy == -error;
	ksft_test_result(rejected, "reject UFFDIO_COPY beyond the file size\n");
	ksft_print_msg("UFFDIO_COPY result=%d errno=%d copy=%lld\n",
		       result, error, (long long)copy.copy);

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
	execl("/proc/self/exe", "userfaultfd_eof_ppps", "--run", NULL);
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
