// SPDX-License-Identifier: GPL-2.0
/*
 * mbind(), set_mempolicy_home_node() and get_mempolicy() from a 4K compat
 * process accept 4K-aligned subpage ranges, and shmem shared policies are
 * applied and looked up at the native page containing a 4K slice.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#define MAPPING_SIZE	(2 * NATIVE_PAGE_SIZE)

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static bool mbind_available(void)
{
	void *probe = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	long result;

	if (probe == MAP_FAILED)
		return true;
	errno = 0;
	result = syscall(__NR_mbind, probe, PROCESS_PAGE_SIZE, MPOL_DEFAULT,
			 NULL, 0, 0);
	munmap(probe, PROCESS_PAGE_SIZE);
	return !(result && errno == ENOSYS);
}

static int run_test(void)
{
	unsigned char *mapping;
	unsigned char *target;
	unsigned long nodemask = 1;
	bool aligned;
	long result;

	ksft_print_header();
	if (!mbind_available())
		ksft_exit_skip("mbind is unavailable (kernel without NUMA)\n");
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	target = (unsigned char *)align_up((uintptr_t)mapping, NATIVE_PAGE_SIZE) +
		 PROCESS_PAGE_SIZE;
	aligned = !((unsigned long)target & (PROCESS_PAGE_SIZE - 1)) &&
		  ((unsigned long)target & (NATIVE_PAGE_SIZE - 1));
	ksft_test_result(aligned,
			 "target is 4K aligned but not 16K aligned\n");
	target[0] = 0xa5;

	errno = 0;
	result = syscall(__NR_mbind, target, PROCESS_PAGE_SIZE, MPOL_DEFAULT,
			 NULL, 0, 0);
	ksft_test_result(!result, "apply MPOL_DEFAULT to one 4K subpage\n");
	ksft_print_msg("MPOL_DEFAULT result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_mbind, target, PROCESS_PAGE_SIZE, MPOL_BIND,
			 &nodemask, sizeof(nodemask) * 8, 0);
	ksft_test_result(!result, "bind one 4K subpage to node 0\n");
	ksft_print_msg("MPOL_BIND result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_set_mempolicy_home_node, target, PROCESS_PAGE_SIZE,
			 0, 0);
	ksft_test_result(!result, "set home node on one 4K subpage\n");
	ksft_print_msg("home-node result=%ld errno=%d\n", result, errno);

	munmap(mapping, MAPPING_SIZE);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
