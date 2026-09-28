// SPDX-License-Identifier: GPL-2.0
/* smaps Swap counts only the 4KB file slices mapped by each shmem VMA. */
#define _GNU_SOURCE
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

FIXTURE(shmem_swap) {
	int fd;
	unsigned char *reservation;
	unsigned char *outside;
};

FIXTURE_SETUP(shmem_swap)
{
	self->fd = -1;
	self->reservation = MAP_FAILED;
	if (!p3s_swap_available())
		SKIP(return, "no swap device is active");
}

FIXTURE_TEARDOWN(shmem_swap)
{
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, 4 * NATIVE_PAGE_SIZE);
	if (self->fd >= 0)
		close(self->fd);
}

static void prepare_swapped_file(struct __test_metadata *_metadata,
				 FIXTURE_DATA(shmem_swap) *self)
{
	unsigned long swapped = 0;
	unsigned int i;

	self->fd = memfd_create("shmem-swap-usage-p3s", MFD_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, 4 * NATIVE_PAGE_SIZE));
	self->reservation = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, self->reservation);
	self->outside = mmap(self->reservation + 2 * NATIVE_PAGE_SIZE,
			     NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		self->fd, NATIVE_PAGE_SIZE);
	ASSERT_NE(MAP_FAILED, self->outside);
	for (i = 0; i < P3S_SLICES; i++)
		self->outside[i * PROCESS_PAGE_SIZE] = 0x40 + i;
	ASSERT_EQ(0, madvise(self->outside, NATIVE_PAGE_SIZE, MADV_PAGEOUT));
	for (i = 0; i < 300; i++) {
		ASSERT_TRUE(p3s_smaps_bytes(self->outside, NATIVE_PAGE_SIZE,
					    "Swap", &swapped));
		if (swapped == NATIVE_PAGE_SIZE)
			break;
		usleep(10000);
	}
	ASSERT_EQ(NATIVE_PAGE_SIZE, swapped);
	ASSERT_EQ(0, munmap(self->outside, NATIVE_PAGE_SIZE));
}

TEST_F(shmem_swap, unrelated_file_page_has_no_swap_charge)
{
	unsigned long swapped;
	void *mapping;

	prepare_swapped_file(_metadata, self);
	mapping = mmap(self->reservation, NATIVE_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, self->fd, 0);
	ASSERT_EQ(self->reservation, mapping);
	ASSERT_TRUE(p3s_smaps_bytes(mapping, NATIVE_PAGE_SIZE, "Swap", &swapped));
	EXPECT_EQ(0, swapped);
}

static void check_slice(struct __test_metadata *_metadata,
			FIXTURE_DATA(shmem_swap) *self, bool private,
			unsigned int slice)
{
	unsigned long swapped;
	void *mapping;

	prepare_swapped_file(_metadata, self);
	mapping = mmap(self->outside, PROCESS_PAGE_SIZE, PROT_READ,
		       (private ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED,
		       self->fd, NATIVE_PAGE_SIZE + slice * PROCESS_PAGE_SIZE);
	ASSERT_EQ(self->outside, mapping);
	ASSERT_TRUE(p3s_smaps_bytes(mapping, PROCESS_PAGE_SIZE, "Swap", &swapped));
	EXPECT_EQ(PROCESS_PAGE_SIZE, swapped);
}

TEST_F(shmem_swap, shared_first_slice_counts_one_process_page)
{
	check_slice(_metadata, self, false, 0);
}

TEST_F(shmem_swap, private_first_slice_counts_one_process_page)
{
	check_slice(_metadata, self, true, 0);
}

TEST_F(shmem_swap, shared_nonzero_slice_counts_one_process_page)
{
	check_slice(_metadata, self, false, 1);
}

TEST_F(shmem_swap, private_nonzero_slice_counts_one_process_page)
{
	check_slice(_metadata, self, true, 1);
}

P3S_HARNESS_4KB_MAIN
