// SPDX-License-Identifier: GPL-2.0
/* Verify slice-offset rejection and file-offset moves before COW. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_harness.h"

static unsigned char *reserve_aligned(size_t usable, void **reservation,
				      size_t *reservation_len)
{
	uintptr_t aligned;
	void *raw;

	*reservation_len = usable + 2 * NATIVE_PAGE_SIZE;
	raw = mmap(NULL, *reservation_len, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)raw + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	*reservation = raw;
	return (unsigned char *)aligned;
}

static bool check_slices(const unsigned char *p, unsigned int nr_slices,
			 unsigned int hole)
{
	unsigned int slice;

	for (slice = 0; slice < nr_slices; slice++) {
		unsigned char expected = slice == hole ? 0 : 0x40 + slice;

		if (p[slice * PROCESS_PAGE_SIZE] != expected ||
		    p[(slice + 1) * PROCESS_PAGE_SIZE - 1] != expected)
			return false;
	}
	return true;
}

FIXTURE(anon_move) {
	void *src_res;
	void *dst_res;
	size_t src_res_len;
	size_t dst_res_len;
	unsigned char *src;
	unsigned char *dst_base;
	size_t length;
};

FIXTURE_VARIANT(anon_move) {
	unsigned int nr_slices;
	unsigned int hole;
};

FIXTURE_VARIANT_ADD(anon_move, full_folio) {
	.nr_slices = P3S_SLICES,
	.hole = P3S_SLICES,
};
FIXTURE_VARIANT_ADD(anon_move, two_folios_with_hole) {
	.nr_slices = 2 * P3S_SLICES,
	.hole = 2,
};

FIXTURE_SETUP(anon_move)
{
	unsigned int slice;

	self->length = variant->nr_slices * PROCESS_PAGE_SIZE;
	self->src = reserve_aligned(self->length, &self->src_res,
				    &self->src_res_len);
	ASSERT_NE(MAP_FAILED, self->src);
	self->dst_base = reserve_aligned(self->length + NATIVE_PAGE_SIZE,
					 &self->dst_res,
					 &self->dst_res_len);
	ASSERT_NE(MAP_FAILED, self->dst_base);
	ASSERT_EQ(0, mprotect(self->src, self->length, PROT_READ | PROT_WRITE));
	for (slice = 0; slice < variant->nr_slices; slice++)
		memset(self->src + slice * PROCESS_PAGE_SIZE, 0x40 + slice,
		       PROCESS_PAGE_SIZE);
	if (variant->hole < variant->nr_slices)
		ASSERT_EQ(0, madvise(self->src + variant->hole * PROCESS_PAGE_SIZE,
				     PROCESS_PAGE_SIZE, MADV_DONTNEED));
}

FIXTURE_TEARDOWN(anon_move)
{
	if (self->dst_res)
		munmap(self->dst_res, self->dst_res_len);
	if (self->src_res)
		munmap(self->src_res, self->src_res_len);
}

TEST_F(anon_move, cross_slice_move_is_rejected_without_data_loss)
{
	unsigned char vec[2 * P3S_SLICES];
	void *moved;

	errno = 0;
	moved = mremap(self->src, self->length, self->length,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst_base + PROCESS_PAGE_SIZE);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EINVAL, errno);
	EXPECT_EQ(0, mincore(self->dst_base + PROCESS_PAGE_SIZE,
			     self->length, vec));
	EXPECT_TRUE(check_slices(self->src, variant->nr_slices, variant->hole));
}

TEST_F(anon_move, same_slice_move_preserves_parent_after_fork_cow)
{
	unsigned int target = variant->nr_slices - 1;
	unsigned char *moved;
	int status;
	pid_t child;

	moved = mremap(self->src, self->length, self->length,
		       MREMAP_MAYMOVE | MREMAP_FIXED, self->dst_base);
	ASSERT_EQ(self->dst_base, moved);
	ASSERT_TRUE(check_slices(moved, variant->nr_slices, variant->hole));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		moved[target * PROCESS_PAGE_SIZE] = 0xa5;
		_exit(moved[target * PROCESS_PAGE_SIZE] != 0xa5 ||
		      !check_slices(moved, target, variant->hole));
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
		EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_TRUE(check_slices(moved, variant->nr_slices, variant->hole));
}

TEST(private_file_cross_slice_move_preserves_cow_and_refault)
{
	const size_t length = NATIVE_PAGE_SIZE;
	void *src_res = NULL, *dst_res = NULL;
	size_t src_res_len, dst_res_len;
	unsigned char *src_base, *src, *dst_base, *moved;
	unsigned char fill[NATIVE_PAGE_SIZE];
	unsigned int slice;
	int fd, status;
	pid_t child;

	src_base = reserve_aligned(length, &src_res, &src_res_len);
	ASSERT_NE(MAP_FAILED, src_base);
	dst_base = reserve_aligned(length + NATIVE_PAGE_SIZE, &dst_res,
				   &dst_res_len);
	ASSERT_NE(MAP_FAILED, dst_base);
	fd = memfd_create("mremap-slice-cow", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, length));
	for (slice = 0; slice < P3S_SLICES; slice++)
		memset(fill + slice * PROCESS_PAGE_SIZE, 0x20 + slice,
		       PROCESS_PAGE_SIZE);
	ASSERT_EQ((ssize_t)sizeof(fill), write(fd, fill, sizeof(fill)));
	src = mmap(src_base, length, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_FIXED, fd, 0);
	ASSERT_EQ(src_base, src);
	for (slice = 0; slice < P3S_SLICES; slice++)
		memset(src + slice * PROCESS_PAGE_SIZE, 0x60 + slice,
		       PROCESS_PAGE_SIZE);
	moved = mremap(src, length, length, MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(dst_base + PROCESS_PAGE_SIZE, moved);
	for (slice = 0; slice < P3S_SLICES; slice++) {
		EXPECT_EQ(0x60 + slice, moved[slice * PROCESS_PAGE_SIZE]);
		EXPECT_EQ(0x60 + slice,
			  moved[(slice + 1) * PROCESS_PAGE_SIZE - 1]);
	}
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		moved[PROCESS_PAGE_SIZE] = 0xa5;
		_exit(moved[0] != 0x60 || moved[PROCESS_PAGE_SIZE] != 0xa5 ||
		      moved[2 * PROCESS_PAGE_SIZE] != 0x62);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
		EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(0x61, moved[PROCESS_PAGE_SIZE]);
	ASSERT_EQ(0, madvise(moved + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	EXPECT_EQ(0x21, moved[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x21, moved[2 * PROCESS_PAGE_SIZE - 1]);
	EXPECT_EQ(0x60, moved[0]);
	EXPECT_EQ(0x62, moved[2 * PROCESS_PAGE_SIZE]);
	close(fd);
	munmap(dst_res, dst_res_len);
	munmap(src_res, src_res_len);
}

P3S_HARNESS_4KB_MAIN
