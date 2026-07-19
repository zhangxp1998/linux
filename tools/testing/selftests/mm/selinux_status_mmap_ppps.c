// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_16K_SIZE	(4 * USER_PAGE_SIZE)
#define POLICY_PATH	"/sys/fs/selinux/policy"
#define STATUS_PATH	"/sys/fs/selinux/status"

struct selinux_kernel_status {
	uint32_t version;
	uint32_t sequence;
	uint32_t enforcing;
	uint32_t policyload;
	uint32_t deny_unknown;
};

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

static int run_test(void)
{
	struct sigaction action = {
		.sa_handler = fault_handler,
	};
	struct stat policy_stat;
	const struct selinux_kernel_status *status;
	unsigned char *reservation;
	unsigned char value = 0;
	bool guards_fault = true;
	bool policy_ready;
	bool policy_tail_faults = false;
	off_t policy_end;
	unsigned int guard;
	void *mapping;
	void *offset_mapping;
	int fd;

	ksft_print_header();
	ksft_set_plan(12);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open(STATUS_PATH, O_RDONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the SELinux status file\n");
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", STATUS_PATH,
				   strerror(errno));

	reservation = mmap(NULL, NATIVE_16K_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(reservation != MAP_FAILED,
			 "reserve one native 16K range as PROT_NONE\n");
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("guard reservation failed: %s\n",
				   strerror(errno));

	mapping = mmap(reservation, USER_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, 0);
	ksft_test_result(mapping == reservation,
			 "map the 4K SELinux status ABI page\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("status mmap failed: %s\n", strerror(errno));

	status = mapping;
	ksft_test_result(status->version == 1,
			 "status mapping exposes ABI version 1\n");
	ksft_test_result(!(status->sequence & 1),
			 "status sequence is stable\n");
	ksft_test_result(status->enforcing <= 1,
			 "status enforcing field is valid\n");

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) ||
	    sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	for (guard = 1; guard < 4; guard++) {
		if (read_byte(reservation + guard * USER_PAGE_SIZE, &value)) {
			ksft_print_msg("guard %u is readable with value %#x\n",
				       guard, value);
			guards_fault = false;
		}
	}
	ksft_test_result(guards_fault,
			 "status mmap does not populate adjacent guard slices\n");

	munmap(reservation, NATIVE_16K_SIZE);
	offset_mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			      USER_PAGE_SIZE);
	ksft_test_result(offset_mapping == MAP_FAILED,
			 "reject a mapping beyond the status page head\n");
	if (offset_mapping != MAP_FAILED)
		munmap(offset_mapping, USER_PAGE_SIZE);

	close(fd);
	fd = open(POLICY_PATH, O_RDONLY | O_CLOEXEC);
	policy_ready = fd >= 0 && !fstat(fd, &policy_stat) && policy_stat.st_size;
	ksft_test_result(policy_ready, "open a non-empty SELinux policy snapshot\n");
	if (!policy_ready)
		ksft_exit_fail_msg("open %s failed: %s\n", POLICY_PATH,
				   strerror(errno));
	policy_end = (policy_stat.st_size + USER_PAGE_SIZE - 1) &
		     ~(USER_PAGE_SIZE - 1);
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       policy_end);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the first 4K process page beyond policy EOF\n");
	if (mapping != MAP_FAILED) {
		policy_tail_faults = !read_byte(mapping, &value);
		munmap(mapping, USER_PAGE_SIZE);
	}
	ksft_test_result(policy_tail_faults,
			 "fault on access beyond policy EOF (size=%lld offset=%lld)\n",
			 (long long)policy_stat.st_size, (long long)policy_end);
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
	execl("/proc/self/exe", "selinux_status_mmap_ppps", "--run", NULL);
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
