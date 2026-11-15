// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process maps the one-page SELinux status ABI without populating the
 * adjacent 4KB slices of its native page, cannot map past the status page,
 * and faults when reading a policy mapping beyond EOF.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

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

static void install_fault_handlers(struct __test_metadata *_metadata)
{
	struct sigaction action = {
		.sa_handler = fault_handler,
	};

	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
}

TEST(selinux_status_mmap)
{
	const struct selinux_kernel_status *status;
	unsigned char *reservation;
	unsigned char value = 0;
	bool guards_fault = true;
	unsigned int guard;
	void *mapping;
	void *offset_mapping;
	int fd;

	fd = p3s_open_fixture(STATUS_PATH, O_RDONLY);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "SELinux status fixture unavailable");
	ASSERT_GE(fd, 0);

	reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED) {
		close(fd);
		ASSERT_NE(MAP_FAILED, reservation);
	}

	mapping = mmap(reservation, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping != reservation) {
		munmap(reservation, NATIVE_PAGE_SIZE);
		close(fd);
		ASSERT_EQ(reservation, mapping);
	}

	status = mapping;
	EXPECT_TRUE(status->version == 1);
	EXPECT_TRUE(!(status->sequence & 1));
	EXPECT_TRUE(status->enforcing <= 1);

	install_fault_handlers(_metadata);
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
}

TEST(selinux_policy_eof_fault)
{
	struct stat policy_stat;
	unsigned char value = 0;
	bool policy_ready;
	off_t policy_end;
	void *mapping;
	int fd;

	fd = p3s_open_fixture(POLICY_PATH, O_RDONLY);
	if (fd < 0 &&
	    (p3s_fixture_unavailable(errno) || errno == EINVAL ||
	     errno == EBUSY))
		SKIP(return, "SELinux policy unavailable");
	ASSERT_GE(fd, 0);

	policy_ready = !fstat(fd, &policy_stat) && policy_stat.st_size > 0;
	if (!policy_ready) {
		close(fd);
		ASSERT_TRUE(policy_ready);
	}

	install_fault_handlers(_metadata);
	policy_end = (policy_stat.st_size + PROCESS_PAGE_SIZE - 1) &
		     ~(PROCESS_PAGE_SIZE - 1);
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       policy_end - PROCESS_PAGE_SIZE);
	EXPECT_TRUE(mapping != MAP_FAILED);
	if (mapping != MAP_FAILED) {
		EXPECT_TRUE(read_byte(mapping, &value));
		munmap(mapping, PROCESS_PAGE_SIZE);
	}

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       policy_end);
	EXPECT_TRUE(mapping != MAP_FAILED);
	if (mapping != MAP_FAILED) {
		EXPECT_TRUE(!read_byte(mapping, &value));
		munmap(mapping, PROCESS_PAGE_SIZE);
	}
	close(fd);
}

P3S_HARNESS_4KB_MAIN
