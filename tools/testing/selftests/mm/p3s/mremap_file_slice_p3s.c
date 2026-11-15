// SPDX-License-Identifier: GPL-2.0
/* Preserve 4KB file offsets and data across fixed and automatic mremap moves. */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define GROW_OLD_SIZE	(4 * NATIVE_PAGE_SIZE)
#define GROW_NEW_SIZE	(2 * GROW_OLD_SIZE)

static void check_fixed_slice_move(struct __test_metadata *_metadata,
				   bool fault_before_move)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	unsigned char *source, *moved, *move_from, *target;
	void *reservation;
	uintptr_t aligned;
	size_t source_len = fault_before_move ? PROCESS_PAGE_SIZE :
			     NATIVE_PAGE_SIZE;
	unsigned int source_slice = fault_before_move ? 0 : 1;
	int fd;

	fd = syscall(__NR_memfd_create, "p3s-mremap-source-slice", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, NATIVE_PAGE_SIZE));
	for (unsigned int i = 0; i < P3S_SLICES; i++) {
		memset(page, 0x41 + i, sizeof(page));
		ASSERT_EQ((ssize_t)sizeof(page),
			  pwrite(fd, page, sizeof(page), i * PROCESS_PAGE_SIZE));
	}
	reservation = mmap(NULL, 5 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	source = mmap((void *)aligned, source_len, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, 0);
	ASSERT_EQ((void *)aligned, source);
	move_from = source + source_slice * PROCESS_PAGE_SIZE;
	if (fault_before_move)
		ASSERT_EQ(0x41, move_from[0]);
	target = (unsigned char *)(aligned + 2 * NATIVE_PAGE_SIZE +
				   (fault_before_move ?
				    2 * PROCESS_PAGE_SIZE : 0));
	moved = mremap(move_from, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, target);
	ASSERT_EQ(target, moved);
	EXPECT_EQ(source_slice * PROCESS_PAGE_SIZE, p3s_maps_offset(moved));
	if (fault_before_move)
		EXPECT_EQ(0x41, moved[0]);
	ASSERT_EQ(0, madvise(moved, PROCESS_PAGE_SIZE, MADV_DONTNEED));
	EXPECT_EQ(0x41 + source_slice, moved[0]);
	EXPECT_EQ(0x41 + source_slice, moved[PROCESS_PAGE_SIZE - 1]);

	munmap(reservation, 5 * NATIVE_PAGE_SIZE);
	close(fd);
}

TEST(fixed_unfaulted_slice_retains_offset_and_refaults_bytes)
{
	check_fixed_slice_move(_metadata, false);
}

TEST(fixed_faulted_slice_retains_offset_and_refaults_bytes)
{
	check_fixed_slice_move(_metadata, true);
}

TEST(automatic_growth_retains_file_offset_and_bytes)
{
	unsigned char *source, *guard, *moved;
	unsigned char head = 0x59, tail = 0x6d;
	void *reservation;
	uintptr_t aligned;
	int fd;

	fd = syscall(__NR_memfd_create, "p3s-mremap-gua", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, GROW_NEW_SIZE + PROCESS_PAGE_SIZE));
	ASSERT_EQ(1, pwrite(fd, &head, 1, PROCESS_PAGE_SIZE));
	ASSERT_EQ(1, pwrite(fd, &tail, 1, GROW_NEW_SIZE));
	reservation = mmap(NULL, GROW_NEW_SIZE + 2 * NATIVE_PAGE_SIZE,
			   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	source = mmap((void *)aligned, GROW_OLD_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, PROCESS_PAGE_SIZE);
	ASSERT_EQ((void *)aligned, source);
	guard = mmap(source + GROW_OLD_SIZE, PROCESS_PAGE_SIZE, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_EQ(source + GROW_OLD_SIZE, guard);
	moved = mremap(source, GROW_OLD_SIZE, GROW_NEW_SIZE, MREMAP_MAYMOVE);
	ASSERT_NE(moved, MAP_FAILED);
	EXPECT_EQ(PROCESS_PAGE_SIZE, p3s_maps_offset(moved));
	EXPECT_EQ(head, moved[0]);
	EXPECT_EQ(tail, moved[GROW_NEW_SIZE - PROCESS_PAGE_SIZE]);

	munmap(moved, GROW_NEW_SIZE);
	munmap(reservation, GROW_NEW_SIZE + 2 * NATIVE_PAGE_SIZE);
	close(fd);
}

FIXTURE(partial_file_move) {
	unsigned char *map, *reservation, *destination;
	int fd;
};

FIXTURE_VARIANT(partial_file_move) {
	bool cross_pte;
};

FIXTURE_VARIANT_ADD(partial_file_move, within_pte_table) { .cross_pte = false };

FIXTURE_VARIANT_ADD(partial_file_move, across_pte_table) { .cross_pte = true };

#define FILE_PTE_SPAN (512UL * PROCESS_PAGE_SIZE)

FIXTURE_SETUP(partial_file_move)
{
	uintptr_t boundary;

	self->map = self->reservation = MAP_FAILED;
	self->fd = memfd_create("p3s-partial-file-move", MFD_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, NATIVE_PAGE_SIZE));
	self->map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->map);
	self->reservation = mmap(NULL, 2 * FILE_PTE_SPAN, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, self->reservation);
	boundary = ((uintptr_t)self->reservation + PROCESS_PAGE_SIZE +
		    FILE_PTE_SPAN - 1) & ~(FILE_PTE_SPAN - 1);
	self->destination = variant->cross_pte ?
		(unsigned char *)boundary - PROCESS_PAGE_SIZE : self->reservation;
}

FIXTURE_TEARDOWN(partial_file_move)
{
	if (self->map != MAP_FAILED)
		munmap(self->map, NATIVE_PAGE_SIZE);
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, 2 * FILE_PTE_SPAN);
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(partial_file_move, middle_cow_slices_remain_independently_writable)
{
	unsigned char *dst = self->destination;
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		self->map[i * PROCESS_PAGE_SIZE] = 0x40 + i;
	ASSERT_EQ(dst, mremap(self->map + PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
			      2 * PROCESS_PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, dst));
	EXPECT_EQ(0x40, self->map[0]);
	EXPECT_EQ(0x43, self->map[3 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x41, dst[0]);
	EXPECT_EQ(0x42, dst[PROCESS_PAGE_SIZE]);
	self->map[0] = 0x60;
	self->map[3 * PROCESS_PAGE_SIZE] = 0x63;
	dst[0] = 0x61;
	dst[PROCESS_PAGE_SIZE] = 0x62;
	EXPECT_EQ(0x60, self->map[0]);
	EXPECT_EQ(0x63, self->map[3 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x61, dst[0]);
	EXPECT_EQ(0x62, dst[PROCESS_PAGE_SIZE]);
}

P3S_HARNESS_4KB_MAIN
