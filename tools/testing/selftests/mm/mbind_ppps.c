// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/memfd.h>
#include <linux/mempolicy.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define MAPPING_SIZE	(2 * NATIVE_PAGE_SIZE)

static int create_shmem_file(void)
{
	int fd = memfd_create("mbind-ppps", MFD_CLOEXEC);

	if (fd >= 0 && ftruncate(fd, 2 * NATIVE_PAGE_SIZE)) {
		close(fd);
		return -1;
	}
	return fd;
}

static long bind_node_zero(void *address, size_t length)
{
	unsigned long nodemask = 1;

	return syscall(__NR_mbind, address, length, MPOL_BIND, &nodemask,
		       sizeof(nodemask) * 8, 0);
}

static long get_address_policy(void *address, int *mode)
{
	return syscall(__NR_get_mempolicy, mode, NULL, 0,
		       (unsigned long)address, MPOL_F_ADDR);
}

static void test_shared_policy_range(void)
{
	void *policy_range;
	void *outside_range;
	long result;
	int mode = -1;
	int fd;

	fd = create_shmem_file();
	if (fd < 0)
		ksft_exit_fail_msg("range memfd setup failed: %s\n",
				   strerror(errno));
	policy_range = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	outside_range = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, NATIVE_PAGE_SIZE);
	if (policy_range == MAP_FAILED || outside_range == MAP_FAILED)
		ksft_exit_fail_msg("range shmem mmap failed: %s\n",
				   strerror(errno));

	errno = 0;
	result = bind_node_zero(policy_range, NATIVE_PAGE_SIZE);
	ksft_test_result(!result,
			 "bind the first native shmem page to node 0\n");
	ksft_print_msg("shared range mbind result=%ld errno=%d\n",
		       result, errno);

	errno = 0;
	result = get_address_policy(outside_range, &mode);
	ksft_print_msg("outside shared policy result=%ld mode=%d errno=%d\n",
		       result, mode, errno);
	ksft_test_result(!result && mode == MPOL_DEFAULT,
			 "shared policy stops at the native page boundary\n");

	munmap(outside_range, USER_PAGE_SIZE);
	munmap(policy_range, NATIVE_PAGE_SIZE);
	close(fd);
}

static void test_shared_policy_slice_lookup(void)
{
	void *policy_page;
	unsigned char *slice_view;
	void *query_address;
	long result;
	int mode = -1;
	int fd;

	fd = create_shmem_file();
	if (fd < 0)
		ksft_exit_fail_msg("lookup memfd setup failed: %s\n",
				   strerror(errno));
	policy_page = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, NATIVE_PAGE_SIZE);
	slice_view = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, USER_PAGE_SIZE);
	if (policy_page == MAP_FAILED || slice_view == MAP_FAILED)
		ksft_exit_fail_msg("lookup shmem mmap failed: %s\n",
				   strerror(errno));

	errno = 0;
	result = bind_node_zero(policy_page, USER_PAGE_SIZE);
	ksft_test_result(!result,
			 "bind file native page 1 through a 4K mapping\n");
	ksft_print_msg("shared lookup mbind result=%ld errno=%d\n",
		       result, errno);

	query_address = slice_view + NATIVE_PAGE_SIZE - USER_PAGE_SIZE;
	errno = 0;
	result = get_address_policy(query_address, &mode);
	ksft_print_msg("slice shared policy result=%ld mode=%d errno=%d\n",
		       result, mode, errno);
	ksft_test_result(!result && mode == MPOL_BIND,
			 "shared policy lookup includes the VMA start slice\n");

	munmap(slice_view, NATIVE_PAGE_SIZE);
	munmap(policy_page, USER_PAGE_SIZE);
	close(fd);
}

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static int run_test(void)
{
	unsigned char *mapping;
	unsigned char *target;
	unsigned long nodemask = 1;
	bool aligned;
	long result;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	target = (unsigned char *)align_up((uintptr_t)mapping, NATIVE_PAGE_SIZE) +
		 USER_PAGE_SIZE;
	aligned = !((unsigned long)target & (USER_PAGE_SIZE - 1)) &&
		  ((unsigned long)target & (NATIVE_PAGE_SIZE - 1));
	ksft_test_result(aligned,
			 "target is 4K aligned but not 16K aligned\n");
	target[0] = 0xa5;

	errno = 0;
	result = syscall(__NR_mbind, target, USER_PAGE_SIZE, MPOL_DEFAULT,
			 NULL, 0, 0);
	ksft_test_result(!result, "apply MPOL_DEFAULT to one 4K subpage\n");
	ksft_print_msg("MPOL_DEFAULT result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_mbind, target, USER_PAGE_SIZE, MPOL_BIND,
			 &nodemask, sizeof(nodemask) * 8, 0);
	ksft_test_result(!result, "bind one 4K subpage to node 0\n");
	ksft_print_msg("MPOL_BIND result=%ld errno=%d\n", result, errno);

	errno = 0;
	result = syscall(__NR_set_mempolicy_home_node, target, USER_PAGE_SIZE,
			 0, 0);
	ksft_test_result(!result, "set home node on one 4K subpage\n");
	ksft_print_msg("home-node result=%ld errno=%d\n", result, errno);

	munmap(mapping, MAPPING_SIZE);
	test_shared_policy_range();
	test_shared_policy_slice_lookup();
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
	execl("/proc/self/exe", "mbind_ppps", "--run", NULL);
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
