// SPDX-License-Identifier: GPL-2.0
/* Verify anonymous phase moves and file-offset moves before COW. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/uio.h>
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

TEST_F(anon_move, cross_slice_move_copies_data)
{
	unsigned char *moved;

	moved = mremap(self->src, self->length, self->length,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(self->dst_base + PROCESS_PAGE_SIZE, moved);
	EXPECT_TRUE(check_slices(moved, variant->nr_slices, variant->hole));
}

TEST_F(anon_move, cross_slice_move_then_fork_cow)
{
	unsigned char *moved;
	pid_t child;
	int status;

	moved = mremap(self->src, self->length, self->length,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(self->dst_base + PROCESS_PAGE_SIZE, moved);
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		if (!check_slices(moved, variant->nr_slices, variant->hole))
			_exit(2);
		moved[0] = 0x77;
		_exit(moved[0] != 0x77);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_TRUE(check_slices(moved, variant->nr_slices, variant->hole));
}

TEST_F(anon_move, cross_slice_move_one_packed_slice)
{
	unsigned char *moved;

	moved = mremap(self->src + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       PROCESS_PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst_base + 2 * PROCESS_PAGE_SIZE);
	ASSERT_EQ(self->dst_base + 2 * PROCESS_PAGE_SIZE, moved);
	EXPECT_EQ(0x41, moved[0]);
	EXPECT_EQ(0x41, moved[PROCESS_PAGE_SIZE - 1]);
	EXPECT_EQ(0x40, self->src[0]);
	EXPECT_EQ(0x43, self->src[3 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x40 + variant->nr_slices - 1,
		  self->src[(variant->nr_slices - 1) * PROCESS_PAGE_SIZE]);
}

TEST_F(anon_move, cross_slice_move_rejects_shared_cow)
{
	int hold[2], status;
	pid_t child;
	char c;
	void *moved;

	ASSERT_EQ(0, pipe(hold));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		close(hold[1]);
		if (read(hold[0], &c, 1) < 0)
			_exit(1);
		_exit(!check_slices(self->src, variant->nr_slices,
				  variant->hole));
	}
	close(hold[0]);
	errno = 0;
	moved = mremap(self->src, self->length, self->length,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       self->dst_base + PROCESS_PAGE_SIZE);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EBUSY, errno);
	EXPECT_TRUE(check_slices(self->src, variant->nr_slices,
				 variant->hole));
	close(hold[1]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
		EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST(cross_slice_move_rejects_vmsplice_reference)
{
	void *src_res = NULL, *dst_res = NULL;
	size_t src_res_len, dst_res_len;
	unsigned char *src, *dst_base, *moved;
	unsigned char copied[PROCESS_PAGE_SIZE];
	struct iovec iov;
	int fds[2];

	src = reserve_aligned(NATIVE_PAGE_SIZE, &src_res, &src_res_len);
	ASSERT_NE(MAP_FAILED, src);
	dst_base = reserve_aligned(2 * NATIVE_PAGE_SIZE, &dst_res,
				   &dst_res_len);
	ASSERT_NE(MAP_FAILED, dst_base);
	ASSERT_EQ(0, mprotect(src, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE));
	memset(src, 0x5a, NATIVE_PAGE_SIZE);
	ASSERT_EQ(0, pipe(fds));
	iov.iov_base = src;
	iov.iov_len = PROCESS_PAGE_SIZE;
	ASSERT_EQ(PROCESS_PAGE_SIZE, vmsplice(fds[1], &iov, 1, 0));
	errno = 0;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EBUSY, errno);
	EXPECT_EQ(0x5a, src[0]);
	ASSERT_EQ(PROCESS_PAGE_SIZE,
		  read(fds[0], copied, sizeof(copied)));
	EXPECT_EQ(0x5a, copied[0]);
	EXPECT_EQ(0x5a, copied[sizeof(copied) - 1]);
	close(fds[0]);
	close(fds[1]);
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(dst_base + PROCESS_PAGE_SIZE, moved);
	EXPECT_EQ(0x5a, moved[0]);
	EXPECT_EQ(0x5a, moved[NATIVE_PAGE_SIZE - 1]);
	munmap(dst_res, dst_res_len);
	munmap(src_res, src_res_len);
}

TEST(cross_slice_move_rolls_back_after_later_folio_is_pinned)
{
	const size_t length = 2 * NATIVE_PAGE_SIZE;
	void *src_res = NULL, *dst_res = NULL;
	size_t src_res_len, dst_res_len;
	unsigned char *src, *dst_base, *moved;
	unsigned char copied[PROCESS_PAGE_SIZE];
	struct iovec iov;
	int fds[2];

	src = reserve_aligned(length, &src_res, &src_res_len);
	ASSERT_NE(MAP_FAILED, src);
	dst_base = reserve_aligned(length + NATIVE_PAGE_SIZE, &dst_res,
				   &dst_res_len);
	ASSERT_NE(MAP_FAILED, dst_base);
	ASSERT_EQ(0, mprotect(src, length, PROT_READ | PROT_WRITE));
	memset(src, 0x51, NATIVE_PAGE_SIZE);
	memset(src + NATIVE_PAGE_SIZE, 0x62, NATIVE_PAGE_SIZE);
	ASSERT_EQ(0, pipe(fds));
	iov.iov_base = src + NATIVE_PAGE_SIZE;
	iov.iov_len = PROCESS_PAGE_SIZE;
	ASSERT_EQ(PROCESS_PAGE_SIZE, vmsplice(fds[1], &iov, 1, 0));
	errno = 0;
	moved = mremap(src, length, length, MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EBUSY, errno);
	EXPECT_EQ(0x51, src[0]);
	EXPECT_EQ(0x51, src[NATIVE_PAGE_SIZE - 1]);
	EXPECT_EQ(0x62, src[NATIVE_PAGE_SIZE]);
	EXPECT_EQ(0x62, src[length - 1]);
	ASSERT_EQ(PROCESS_PAGE_SIZE,
		  read(fds[0], copied, sizeof(copied)));
	EXPECT_EQ(0x62, copied[0]);
	close(fds[0]);
	close(fds[1]);
	moved = mremap(src, length, length, MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(dst_base + PROCESS_PAGE_SIZE, moved);
	EXPECT_EQ(0x51, moved[0]);
	EXPECT_EQ(0x62, moved[length - 1]);
	munmap(dst_res, dst_res_len);
	munmap(src_res, src_res_len);
}

TEST(cross_slice_readonly_move_preserves_data_and_protection)
{
	void *src_res = NULL, *dst_res = NULL;
	size_t src_res_len, dst_res_len;
	unsigned char *src, *dst_base, *moved;
	int status;
	pid_t child;
	unsigned int i;

	src = reserve_aligned(NATIVE_PAGE_SIZE, &src_res, &src_res_len);
	ASSERT_NE(MAP_FAILED, src);
	dst_base = reserve_aligned(2 * NATIVE_PAGE_SIZE, &dst_res,
				   &dst_res_len);
	ASSERT_NE(MAP_FAILED, dst_base);
	ASSERT_EQ(0, mprotect(src, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE));
	for (i = 0; i < P3S_SLICES; i++)
		memset(src + i * PROCESS_PAGE_SIZE, 0x40 + i,
		       PROCESS_PAGE_SIZE);
	ASSERT_EQ(0, mprotect(src, NATIVE_PAGE_SIZE, PROT_READ));
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED,
		       dst_base + PROCESS_PAGE_SIZE);
	ASSERT_EQ(dst_base + PROCESS_PAGE_SIZE, moved);
	EXPECT_TRUE(check_slices(moved, P3S_SLICES, P3S_SLICES));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		moved[0] = 0;
		_exit(1);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFSIGNALED(status));
	if (WIFSIGNALED(status))
		EXPECT_EQ(SIGSEGV, WTERMSIG(status));
	munmap(dst_res, dst_res_len);
	munmap(src_res, src_res_len);
}

struct concurrent_move {
	unsigned char *src;
	unsigned char *dst;
	atomic_bool *start;
	void *moved;
};

static void *cross_slice_move_thread(void *arg)
{
	struct concurrent_move *move = arg;

	while (!atomic_load_explicit(move->start, memory_order_acquire))
		sched_yield();
	move->moved = mremap(move->src, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MREMAP_MAYMOVE | MREMAP_FIXED, move->dst);
	return NULL;
}

TEST(cross_slice_concurrent_moves_from_one_packed_folio)
{
	void *src_res = NULL, *dst_res = NULL;
	size_t src_res_len, dst_res_len;
	unsigned char *src, *dst_base;
	struct concurrent_move moves[2];
	pthread_t threads[2];
	atomic_bool start = ATOMIC_VAR_INIT(false);
	unsigned int i;

	src = reserve_aligned(NATIVE_PAGE_SIZE, &src_res, &src_res_len);
	ASSERT_NE(MAP_FAILED, src);
	dst_base = reserve_aligned(3 * NATIVE_PAGE_SIZE, &dst_res,
				   &dst_res_len);
	ASSERT_NE(MAP_FAILED, dst_base);
	ASSERT_EQ(0, mprotect(src, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE));
	for (i = 0; i < P3S_SLICES; i++)
		memset(src + i * PROCESS_PAGE_SIZE, 0x50 + i,
		       PROCESS_PAGE_SIZE);
	moves[0] = (struct concurrent_move) {
		.src = src + PROCESS_PAGE_SIZE,
		.dst = dst_base + 2 * PROCESS_PAGE_SIZE,
		.start = &start,
	};
	moves[1] = (struct concurrent_move) {
		.src = src + 2 * PROCESS_PAGE_SIZE,
		.dst = dst_base + NATIVE_PAGE_SIZE,
		.start = &start,
	};
	ASSERT_EQ(0, pthread_create(&threads[0], NULL,
				    cross_slice_move_thread, &moves[0]));
	ASSERT_EQ(0, pthread_create(&threads[1], NULL,
				    cross_slice_move_thread, &moves[1]));
	atomic_store_explicit(&start, true, memory_order_release);
	ASSERT_EQ(0, pthread_join(threads[0], NULL));
	ASSERT_EQ(0, pthread_join(threads[1], NULL));
	ASSERT_EQ(moves[0].dst, moves[0].moved);
	ASSERT_EQ(moves[1].dst, moves[1].moved);
	EXPECT_EQ(0x50, src[0]);
	EXPECT_EQ(0x53, src[3 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x51, moves[0].dst[0]);
	EXPECT_EQ(0x51, moves[0].dst[PROCESS_PAGE_SIZE - 1]);
	EXPECT_EQ(0x52, moves[1].dst[0]);
	EXPECT_EQ(0x52, moves[1].dst[PROCESS_PAGE_SIZE - 1]);
	munmap(dst_res, dst_res_len);
	munmap(src_res, src_res_len);
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

P3S_HARNESS_COMPAT_MAIN
