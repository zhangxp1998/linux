// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process maps the one-page SELinux status ABI without
 * populating the adjacent 4K slices of its native page, cannot map past
 * the status page, and faults when reading a policy mapping beyond EOF.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#include "p3s_harness.h"

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

TEST(p3s_selinux_status_mmap)
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

	fd = p3s_open_fixture_or_skip(STATUS_PATH, O_RDONLY);
	EXPECT_TRUE(fd >= 0);

	reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	EXPECT_TRUE(reservation != MAP_FAILED);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("guard reservation failed: %s\n",
				   strerror(errno));

	mapping = mmap(reservation, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, 0);
	EXPECT_TRUE(mapping == reservation);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("status mmap failed: %s\n", strerror(errno));

	status = mapping;
	EXPECT_TRUE(status->version == 1);
	EXPECT_TRUE(!(status->sequence & 1));
	EXPECT_TRUE(status->enforcing <= 1);

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) ||
	    sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	for (guard = 1; guard < P3S_SLICES; guard++) {
		if (read_byte(reservation + guard * PROCESS_PAGE_SIZE, &value)) {
			ksft_print_msg("guard %u is readable with value %#x\n",
				       guard, value);
			guards_fault = false;
		}
	}
	EXPECT_TRUE(guards_fault);

	munmap(reservation, NATIVE_PAGE_SIZE);
	offset_mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			      PROCESS_PAGE_SIZE);
	EXPECT_TRUE(offset_mapping == MAP_FAILED);
	if (offset_mapping != MAP_FAILED)
		munmap(offset_mapping, PROCESS_PAGE_SIZE);

	offset_mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			      PROCESS_PAGE_SIZE);
	EXPECT_TRUE(offset_mapping == MAP_FAILED);
	if (offset_mapping != MAP_FAILED)
		munmap(offset_mapping, NATIVE_PAGE_SIZE);

	close(fd);
	fd = p3s_open_fixture_or_skip(POLICY_PATH, O_RDONLY);
	policy_ready = !fstat(fd, &policy_stat) && policy_stat.st_size;
	EXPECT_TRUE(policy_ready);
	if (!policy_ready)
		ksft_exit_fail_msg("open %s failed: %s\n", POLICY_PATH,
				   strerror(errno));
	policy_end = (policy_stat.st_size + PROCESS_PAGE_SIZE - 1) &
		     ~(PROCESS_PAGE_SIZE - 1);
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       policy_end);
	EXPECT_TRUE(mapping != MAP_FAILED);
	if (mapping != MAP_FAILED) {
		policy_tail_faults = !read_byte(mapping, &value);
		munmap(mapping, PROCESS_PAGE_SIZE);
	}
	EXPECT_TRUE(policy_tail_faults);
	close(fd);
}

P3S_HARNESS_COMPAT_MAIN
