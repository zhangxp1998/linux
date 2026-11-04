// SPDX-License-Identifier: GPL-2.0

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>

#include "p3s_test.h"
#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "remap_pfn_cow_p3s.h"

#define TEST_MARKER	0x93
#define TEST_SLICE	2U
#define RESERVATION_SIZE	(3 * NATIVE_PAGE_SIZE)

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

FIXTURE(pfn_remap) {
	int fd;
};

FIXTURE_SETUP(pfn_remap)
{
	self->fd = p3s_open_fixture("/dev/remap_pfn_cow_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "private PFN remap fixture is unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(pfn_remap)
{
	close(self->fd);
}

static unsigned char *map_last_native_page(int fd, void **reservation)
{
	unsigned char *target, *mapping;

	*reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return MAP_FAILED;
	target = (void *)(((uintptr_t)*reservation + NATIVE_PAGE_SIZE - 1) &
			   ~(NATIVE_PAGE_SIZE - 1));
	mapping = mmap(target, getpagesize(), PROT_READ, MAP_SHARED | MAP_FIXED,
		       fd, NATIVE_PAGE_SIZE);
	if (mapping != target) {
		munmap(*reservation, RESERVATION_SIZE);
		return MAP_FAILED;
	}
	return mapping;
}

TEST_F(pfn_remap, shared_zero_offset_reads_first_slice)
{
	unsigned char *mapping = mmap(NULL, getpagesize(), PROT_READ,
				      MAP_SHARED, self->fd, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(TEST_MARKER - TEST_SLICE, mapping[0]);
	munmap(mapping, getpagesize());
}

TEST_F(pfn_remap, private_mapping_reads_physical_slice)
{
	unsigned char *mapping = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
				      MAP_PRIVATE, self->fd, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(TEST_MARKER - TEST_SLICE, mapping[0]);
	munmap(mapping, getpagesize());
}

TEST_F(pfn_remap, private_vma_keeps_full_physical_offset)
{
	struct remap_pfn_cow_p3s_probe probe = {};
	unsigned char *mapping = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
				      MAP_PRIVATE, self->fd, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	probe.address = (uintptr_t)mapping;
	ASSERT_EQ(0, ioctl(self->fd, REMAP_PFN_COW_P3S_IOCTL, &probe));
	EXPECT_EQ(0, probe.slice);
	EXPECT_TRUE(probe.offset_matches);
	munmap(mapping, getpagesize());
}

TEST_F(pfn_remap, non_native_aligned_offset_rejected)
{
	unsigned char *mapping;
	int error;

	if (!p3s_is_4kb_process())
		SKIP(return, "requires P3S 4K process");
	mapping = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED,
		       self->fd, TEST_SLICE * PROCESS_PAGE_SIZE);
	error = errno;
	if (mapping != MAP_FAILED)
		munmap(mapping, getpagesize());
	EXPECT_EQ(mapping, MAP_FAILED);
	EXPECT_EQ(EINVAL, error);
}

TEST_F(pfn_remap, last_page_tail_keeps_data)
{
	void *reservation;
	unsigned char *mapping;

	if (!p3s_is_4kb_process())
		SKIP(return, "requires P3S 4K process");
	mapping = map_last_native_page(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(TEST_MARKER - TEST_SLICE, mapping[0]);
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_remap, last_page_tail_does_not_map_guard_slices)
{
	struct sigaction action = { .sa_handler = fault_handler };
	unsigned char *mapping, value;
	void *reservation;
	unsigned int guard;

	if (!p3s_is_4kb_process())
		SKIP(return, "requires P3S 4K process");
	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
	mapping = map_last_native_page(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	for (guard = 1; guard < P3S_SLICES; guard++)
		EXPECT_FALSE(read_byte(mapping + guard * PROCESS_PAGE_SIZE, &value));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_remap, offset_beyond_last_native_page_rejected)
{
	unsigned char *mapping, *target;
	void *reservation;
	int error;

	if (!p3s_is_4kb_process())
		SKIP(return, "requires P3S 4K process");
	reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	target = (void *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
			  ~(NATIVE_PAGE_SIZE - 1));
	mapping = mmap(target, getpagesize(), PROT_READ, MAP_SHARED | MAP_FIXED,
		       self->fd, 2 * NATIVE_PAGE_SIZE);
	error = errno;
	if (mapping != MAP_FAILED)
		munmap(mapping, getpagesize());
	EXPECT_EQ(mapping, MAP_FAILED);
	EXPECT_EQ(EINVAL, error);
	munmap(reservation, RESERVATION_SIZE);
}

P3S_HARNESS_4KB_MAIN
