// SPDX-License-Identifier: GPL-2.0
/* vmf_insert_pfn_prot() atomically inserts/verifies the in-VMA PFN slices. */

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "pfn_insert_p3s.h"

#define RESERVATION_SIZE	(3 * NATIVE_PAGE_SIZE)
#define COMPAT_PMD_SIZE	(512 * PROCESS_PAGE_SIZE)
#define FIRST_MARKER	0x51
#define WRONG_MARKER	0x61

static unsigned char *fixture_map_len(int fd, off_t offset,
				      size_t virtual_shift, size_t length,
				      void **reservation)
{
	unsigned char *target;

	*reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return MAP_FAILED;
	target = (void *)(((uintptr_t)*reservation + NATIVE_PAGE_SIZE - 1) &
			  ~(NATIVE_PAGE_SIZE - 1));
	target += virtual_shift;
	return mmap(target, length, PROT_READ,
		    MAP_SHARED | MAP_FIXED, fd, offset);
}

static unsigned char *fixture_map(int fd, off_t offset, size_t virtual_shift,
				  void **reservation)
{
	return fixture_map_len(fd, offset, virtual_shift, NATIVE_PAGE_SIZE,
			       reservation);
}

static bool inspect_mask(int fd, const unsigned char *mapping,
			 unsigned int expected)
{
	struct pfn_insert_p3s_request request = {
		.address = (uintptr_t)mapping,
	};

	if (ioctl(fd, PFN_INSERT_P3S_INSPECT, &request))
		return false;
	if (request.present_mask != expected)
		ksft_print_msg("present mask %#x, expected %#x\n",
			       request.present_mask, expected);
	return request.present_mask == expected;
}

static bool page_markers(const unsigned char *mapping, unsigned char first,
			 unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (mapping[i * PROCESS_PAGE_SIZE] != first + i)
			return false;
	}
	return true;
}

FIXTURE(pfn_insert)
{
	int fd;
};

FIXTURE_SETUP(pfn_insert)
{
	self->fd = p3s_open_fixture("/dev/pfn_insert_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "pfn_insert fixture unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(pfn_insert)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(pfn_insert, one_fault_installs_matching_native_page)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	mapping = fixture_map(self->fd, 0, 0, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(FIRST_MARKER, mapping[0]);
	EXPECT_TRUE(inspect_mask(self->fd, mapping, (1U << P3S_SLICES) - 1));
	EXPECT_TRUE(page_markers(mapping, FIRST_MARKER, P3S_SLICES));
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	EXPECT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, shifted_virtual_address)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map(self->fd, 0, PROCESS_PAGE_SIZE, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_TRUE(inspect_mask(self->fd, mapping, (1U << P3S_SLICES) - 1));
	EXPECT_TRUE(page_markers(mapping, FIRST_MARKER, P3S_SLICES));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, crosses_compat_pte_tables)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	reservation = mmap(NULL, 2 * COMPAT_PMD_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	mapping = (void *)((((uintptr_t)reservation + COMPAT_PMD_SIZE - 1) &
			    ~(COMPAT_PMD_SIZE - 1)) - PROCESS_PAGE_SIZE);
	mapping = mmap(mapping, NATIVE_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_TRUE(inspect_mask(self->fd, mapping, (1U << P3S_SLICES) - 1));
	EXPECT_TRUE(page_markers(mapping, FIRST_MARKER, P3S_SLICES));
	munmap(reservation, 2 * COMPAT_PMD_SIZE);
}

TEST_F(pfn_insert, partial_quartet_rejected_without_changes)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map(self->fd, NATIVE_PAGE_SIZE, 0, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(inspect_mask(self->fd, mapping, 1));
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	errno = 0;
	EXPECT_EQ(-1, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_TRUE(inspect_mask(self->fd, mapping, 1));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, wrong_quartet_rejected_without_changes)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map(self->fd, 2 * NATIVE_PAGE_SIZE, 0,
			      &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(inspect_mask(self->fd, mapping, (1U << P3S_SLICES) - 1));
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	errno = 0;
	EXPECT_EQ(-1, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_TRUE(page_markers(mapping, WRONG_MARKER, P3S_SLICES));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, tail_window_inserts_only_in_vma_slices)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map_len(self->fd, 0, 0, 3 * PROCESS_PAGE_SIZE,
				  &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_TRUE(inspect_mask(self->fd, mapping, 7));
	EXPECT_TRUE(page_markers(mapping, FIRST_MARKER, 3));
	EXPECT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_FALSE(p3s_page_present(mapping + 3 * PROCESS_PAGE_SIZE));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, offset_window_inserts_only_middle_slices)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map(self->fd, 0, 0, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_EQ(0, munmap(mapping, PROCESS_PAGE_SIZE));
	ASSERT_EQ(0, munmap(mapping + 3 * PROCESS_PAGE_SIZE,
			    PROCESS_PAGE_SIZE));
	mapping += PROCESS_PAGE_SIZE;
	request.address = (uintptr_t)mapping;
	ASSERT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_TRUE(inspect_mask(self->fd, mapping, 3));
	EXPECT_TRUE(page_markers(mapping, FIRST_MARKER + 1, 2));
	EXPECT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_FALSE(p3s_page_present(mapping + 2 * PROCESS_PAGE_SIZE));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, last_slice_without_preceding_slices)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map(self->fd, 0, 0, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_EQ(0, munmap(mapping, 3 * PROCESS_PAGE_SIZE));
	mapping += 3 * PROCESS_PAGE_SIZE;
	request.address = (uintptr_t)mapping;
	ASSERT_EQ(0, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_TRUE(inspect_mask(self->fd, mapping, 1));
	EXPECT_EQ(FIRST_MARKER + 3, mapping[0]);
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, partial_tail_rejected_without_changes)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map_len(self->fd, NATIVE_PAGE_SIZE, 0,
				  3 * PROCESS_PAGE_SIZE, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(inspect_mask(self->fd, mapping, 1));
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	errno = 0;
	EXPECT_EQ(-1, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_TRUE(inspect_mask(self->fd, mapping, 1));
	munmap(reservation, RESERVATION_SIZE);
}

TEST_F(pfn_insert, wrong_tail_rejected_without_changes)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;

	if (!p3s_is_4kb_process())
		SKIP(return, "needs 4K P3S geometry");
	mapping = fixture_map_len(self->fd, 2 * NATIVE_PAGE_SIZE, 0,
				  3 * PROCESS_PAGE_SIZE, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(inspect_mask(self->fd, mapping, 7));
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	errno = 0;
	EXPECT_EQ(-1, ioctl(self->fd, PFN_INSERT_P3S_IOCTL, &request));
	EXPECT_EQ(EBUSY, errno);
	EXPECT_TRUE(page_markers(mapping, WRONG_MARKER, 3));
	munmap(reservation, RESERVATION_SIZE);
}

P3S_HARNESS_4KB_MAIN
