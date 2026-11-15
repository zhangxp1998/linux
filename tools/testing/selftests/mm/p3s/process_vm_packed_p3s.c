// SPDX-License-Identifier: GPL-2.0
/* process_vm_readv/writev cross from slice 1 of one packed folio into the next. */
#define _GNU_SOURCE
#include <sys/uio.h>
#include "p3s_slice_test.h"
#include "p3s_harness.h"

FIXTURE(process_vm_packed) {
	unsigned char *map, *reservation;
	struct iovec remote;
};

FIXTURE_SETUP(process_vm_packed)
{
	uint64_t pfn0[P3S_SLICES], pfn1[P3S_SLICES];

	self->reservation = MAP_FAILED;
	self->map = map_aligned(2 * NATIVE_PAGE_SIZE, &self->reservation);
	ASSERT_NE(MAP_FAILED, self->map);
	fill_slices(self->map, 0x31);
	fill_slices(self->map + NATIVE_PAGE_SIZE, 0x41);
	ASSERT_TRUE(read_pfns(self->map, pfn0));
	ASSERT_TRUE(read_pfns(self->map + NATIVE_PAGE_SIZE, pfn1));
	if (!pfn0[0] || !pfn1[0])
		SKIP(return, "PFNs unavailable");
	ASSERT_TRUE(same_pfn(pfn0));
	ASSERT_TRUE(same_pfn(pfn1));
	ASSERT_NE(pfn0[0], pfn1[0]);
	self->remote = (struct iovec) {
		self->map + PROCESS_PAGE_SIZE, NATIVE_PAGE_SIZE
	};
}

FIXTURE_TEARDOWN(process_vm_packed)
{
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST_F(process_vm_packed, readv_copies_each_slice)
{
	const unsigned char expected[P3S_SLICES] = { 0x32, 0x33, 0x34, 0x41 };
	unsigned char buffer[NATIVE_PAGE_SIZE];
	struct iovec local = { buffer, sizeof(buffer) };
	unsigned int i;

	ASSERT_EQ(sizeof(buffer), process_vm_readv(getpid(), &local, 1,
						   &self->remote, 1, 0));
	for (i = 0; i < P3S_SLICES; i++)
		EXPECT_TRUE(all_bytes_are(buffer + i * PROCESS_PAGE_SIZE,
					  PROCESS_PAGE_SIZE, expected[i]));
}

TEST_F(process_vm_packed, writev_updates_each_slice)
{
	unsigned char buffer[NATIVE_PAGE_SIZE];
	struct iovec local = { buffer, sizeof(buffer) };
	unsigned int i;

	fill_slices(buffer, 0x61);
	ASSERT_EQ(sizeof(buffer), process_vm_writev(getpid(), &local, 1,
						    &self->remote, 1, 0));
	EXPECT_TRUE(all_bytes_are(self->map, PROCESS_PAGE_SIZE, 0x31));
	EXPECT_TRUE(check_slices(self->map + PROCESS_PAGE_SIZE, 0x61));
	for (i = 1; i < P3S_SLICES; i++)
		EXPECT_TRUE(all_bytes_are(self->map + NATIVE_PAGE_SIZE +
					  i * PROCESS_PAGE_SIZE,
					  PROCESS_PAGE_SIZE, 0x41 + i));
}

P3S_HARNESS_4KB_MAIN
