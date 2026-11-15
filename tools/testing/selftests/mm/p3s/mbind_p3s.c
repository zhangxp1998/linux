// SPDX-License-Identifier: GPL-2.0
/*
 * mbind(), set_mempolicy_home_node() and get_mempolicy() from a 4K compat
 * process accept 4K-aligned subpage ranges, and shmem shared policies are
 * applied at the native-page granularity.
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

#define MAPPING_SIZE (2 * NATIVE_PAGE_SIZE)

static int create_shmem_file(void)
{
	int fd = memfd_create("mbind-p3s", MFD_CLOEXEC);

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

FIXTURE(mbind_anon) {
	unsigned char *mapping;
	unsigned char *target;
};

FIXTURE_SETUP(mbind_anon)
{
	if (!mbind_available())
		SKIP(return, "mbind is unavailable (kernel without NUMA)");
	self->mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	self->target = (unsigned char *)align_up((uintptr_t)self->mapping,
						    NATIVE_PAGE_SIZE) +
		       PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, (uintptr_t)self->target & (PROCESS_PAGE_SIZE - 1));
	ASSERT_NE(0, (uintptr_t)self->target & (NATIVE_PAGE_SIZE - 1));
	self->target[0] = 0xa5;
}

FIXTURE_TEARDOWN(mbind_anon)
{
	munmap(self->mapping, MAPPING_SIZE);
}

TEST_F(mbind_anon, default_policy_accepts_4k_subpage)
{
	EXPECT_EQ(0, syscall(__NR_mbind, self->target, PROCESS_PAGE_SIZE,
			     MPOL_DEFAULT, NULL, 0, 0));
}

TEST_F(mbind_anon, bind_accepts_4k_subpage)
{
	int mode = -1;

	ASSERT_EQ(0, bind_node_zero(self->target, PROCESS_PAGE_SIZE));
	ASSERT_EQ(0, get_address_policy(self->target, &mode));
	EXPECT_EQ(MPOL_BIND, mode);
	ASSERT_EQ(0, get_address_policy(self->target - PROCESS_PAGE_SIZE, &mode));
	EXPECT_EQ(MPOL_DEFAULT, mode);
	ASSERT_EQ(0, get_address_policy(self->target + PROCESS_PAGE_SIZE, &mode));
	EXPECT_EQ(MPOL_DEFAULT, mode);
}

TEST_F(mbind_anon, home_node_accepts_4k_subpage)
{
	ASSERT_EQ(0, bind_node_zero(self->target, PROCESS_PAGE_SIZE));
	EXPECT_EQ(0, syscall(__NR_set_mempolicy_home_node, self->target,
			     PROCESS_PAGE_SIZE, 0, 0));
}

FIXTURE(mbind_shared) {
	int fd;
	void *policy_range;
	void *outside_range;
};

FIXTURE_SETUP(mbind_shared)
{
	if (!mbind_available())
		SKIP(return, "mbind is unavailable (kernel without NUMA)");
	self->fd = create_shmem_file();
	ASSERT_GE(self->fd, 0);
	self->policy_range = mmap(NULL, NATIVE_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  self->fd, 0);
	ASSERT_NE(self->policy_range, MAP_FAILED);
	self->outside_range = mmap(NULL, PROCESS_PAGE_SIZE,
				   PROT_READ | PROT_WRITE, MAP_SHARED,
				   self->fd, NATIVE_PAGE_SIZE);
	ASSERT_NE(self->outside_range, MAP_FAILED);
}

FIXTURE_TEARDOWN(mbind_shared)
{
	munmap(self->outside_range, PROCESS_PAGE_SIZE);
	munmap(self->policy_range, NATIVE_PAGE_SIZE);
	close(self->fd);
}

TEST_F(mbind_shared, bind_first_native_shmem_page)
{
	EXPECT_EQ(0, bind_node_zero(self->policy_range, NATIVE_PAGE_SIZE));
}

TEST_F(mbind_shared, policy_stops_at_native_boundary)
{
	int mode = -1;

	ASSERT_EQ(0, bind_node_zero(self->policy_range, NATIVE_PAGE_SIZE));
	ASSERT_EQ(0, get_address_policy(self->outside_range, &mode));
	EXPECT_EQ(MPOL_DEFAULT, mode);
}

TEST_F(mbind_shared, sliced_vma_finds_policy_of_mapped_page)
{
	unsigned char *sliced;
	int mode = -1;

	/* Bind file page 1 only. */
	ASSERT_EQ(0, bind_node_zero(self->outside_range, PROCESS_PAGE_SIZE));
	/* At file offset 4KB, the fourth 4KB slice maps file page 1. */
	sliced = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		      self->fd, PROCESS_PAGE_SIZE);
	ASSERT_NE(MAP_FAILED, sliced);
	ASSERT_EQ(0, get_address_policy(sliced + 3 * PROCESS_PAGE_SIZE, &mode));
	EXPECT_EQ(MPOL_BIND, mode);
	ASSERT_EQ(0, get_address_policy(sliced, &mode));
	EXPECT_EQ(MPOL_DEFAULT, mode);
	munmap(sliced, NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
