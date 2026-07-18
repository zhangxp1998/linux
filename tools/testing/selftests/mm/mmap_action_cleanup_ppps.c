// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define RESERVE_SIZE	(5 * NATIVE_PAGE_SIZE)

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool range_is_mapped(void *address, size_t length)
{
	unsigned char vec[4];

	return !mincore(address, length, vec);
}

static int run_test(void)
{
	unsigned char *mapping;
	unsigned char *target;
	void *result;
	int fd;

	ksft_print_header();
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/gup_retry_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open mmap cleanup test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, RESERVE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("reserve mapping failed: %s\n",
				   strerror(errno));
	target = (unsigned char *)align_up((uintptr_t)mapping,
					    NATIVE_PAGE_SIZE) +
		 NATIVE_PAGE_SIZE;
	ksft_test_result(!((uintptr_t)target % NATIVE_PAGE_SIZE) &&
			 target - USER_PAGE_SIZE >= mapping &&
			 target + NATIVE_PAGE_SIZE <= mapping + RESERVE_SIZE,
			 "choose a native-aligned 4K replacement range\n");

	memset(target - USER_PAGE_SIZE, 0x11, USER_PAGE_SIZE);
	memset(target + USER_PAGE_SIZE, 0x22,
	       NATIVE_PAGE_SIZE - USER_PAGE_SIZE);
	errno = 0;
	result = mmap(target, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, 0);
	ksft_test_result(result == MAP_FAILED && errno == EIO,
			 "mmap success hook rejects the new 4K VMA\n");
	ksft_print_msg("mmap result=%p errno=%d\n", result, errno);

	errno = 0;
	ksft_test_result(!range_is_mapped(target, USER_PAGE_SIZE) &&
			 errno == ENOMEM,
			 "failed mmap leaves its 4K range unmapped\n");
	ksft_test_result(range_is_mapped(target - USER_PAGE_SIZE,
					 USER_PAGE_SIZE),
			 "failed mmap preserves the preceding 4K page\n");
	ksft_test_result(range_is_mapped(target + USER_PAGE_SIZE,
					 NATIVE_PAGE_SIZE - USER_PAGE_SIZE),
			 "failed mmap preserves the following 12K range\n");

	munmap(mapping, RESERVE_SIZE);
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
	execl("/proc/self/exe", "mmap_action_cleanup_ppps", "--run", NULL);
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
