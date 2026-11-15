// SPDX-License-Identifier: GPL-2.0
/*
 * vmsplice(2) of a 4KB process's memory into a pipe must queue and read back
 * the exact requested bytes across every 4KB slice, unaligned range, and
 * native-page boundary, for both anonymous folios (including partial unmaps
 * and live pipe refaults after MADV_DONTNEED) and sliced memfd mappings where
 * vm_slice_off is non-zero.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

static bool splice_and_check(unsigned char *base, unsigned long offset,
			     size_t len, const char *what)
{
	struct iovec iov = { .iov_base = base + offset, .iov_len = len };
	unsigned char *readback = malloc(len);
	int pipefd[2], queued = -1;
	ssize_t ret;
	bool ok = true;

	if (!readback) {
		ksft_print_msg("malloc: %s\n", strerror(errno));
		return false;
	}
	if (pipe(pipefd)) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		free(readback);
		return false;
	}
	ksft_print_msg("%s: vmsplice %zu bytes at window offset %lu\n", what,
		       len, offset);
	fflush(stdout);
	ret = vmsplice(pipefd[1], &iov, 1, 0);
	if (ret != (ssize_t)len) {
		ksft_print_msg("%s: vmsplice returned %zd (%s)\n", what, ret,
			       ret < 0 ? strerror(errno) : "short");
		ok = false;
	}
	if (ioctl(pipefd[0], FIONREAD, &queued) || queued != (int)len) {
		ksft_print_msg("%s: pipe holds %d bytes, expected %zu\n", what,
			       queued, len);
		ok = false;
	}
	if (ok) {
		if (!read_full(pipefd[0], readback, len) ||
		    memcmp(readback, base + offset, len)) {
			ksft_print_msg("%s: pipe contents differ from the buffer\n",
				       what);
			ok = false;
		}
	}
	close(pipefd[0]);
	close(pipefd[1]);
	free(readback);
	return ok;
}

TEST(p3s_vmsplice_slice)
{
	unsigned char *reservation, *base;
	unsigned int slice;
	char what[64];
	unsigned long i;

	base = map_aligned(2 * NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(MAP_FAILED, base);
	for (i = 0; i < 2 * NATIVE_PAGE_SIZE; i++)
		base[i] = (unsigned char)(i * 7 + i / PROCESS_PAGE_SIZE);

	for (slice = 0; slice < P3S_SLICES; slice++) {
		snprintf(what, sizeof(what), "slice %u", slice);
		EXPECT_TRUE(splice_and_check(base, slice * PROCESS_PAGE_SIZE,
					     PROCESS_PAGE_SIZE, what));
		fflush(stdout);
	}
	EXPECT_TRUE(splice_and_check(base, PROCESS_PAGE_SIZE + 100,
				     2 * PROCESS_PAGE_SIZE, "unaligned"));
	fflush(stdout);
	EXPECT_TRUE(splice_and_check(base, 3 * PROCESS_PAGE_SIZE,
				     NATIVE_PAGE_SIZE, "crossing"));
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST(vmsplice_sliced_memfd_mapping)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	unsigned char *reservation, *mapped;
	unsigned int slice, i;
	char what[64];
	int fd;

	fd = memfd_create("p3s-vmsplice-file", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, 2 * NATIVE_PAGE_SIZE));
	for (slice = 0; slice < 2 * P3S_SLICES; slice++) {
		for (i = 0; i < PROCESS_PAGE_SIZE; i++)
			page[i] = (unsigned char)(i * 11 + slice + 1);
		ASSERT_TRUE(pwrite_full(fd, page, sizeof(page),
					(off_t)slice * PROCESS_PAGE_SIZE));
	}

	mapped = map_aligned_prot(NATIVE_PAGE_SIZE, PROT_NONE, &reservation);
	ASSERT_NE(MAP_FAILED, mapped);
	ASSERT_EQ(mapped, mmap(mapped, NATIVE_PAGE_SIZE, PROT_READ,
			       MAP_SHARED | MAP_FIXED, fd, PROCESS_PAGE_SIZE));

	for (slice = 0; slice < P3S_SLICES; slice++) {
		snprintf(what, sizeof(what), "memfd-slice %u", slice);
		EXPECT_TRUE(splice_and_check(mapped, slice * PROCESS_PAGE_SIZE,
					     PROCESS_PAGE_SIZE, what));
	}
	EXPECT_TRUE(splice_and_check(mapped, PROCESS_PAGE_SIZE + 100,
				     2 * PROCESS_PAGE_SIZE,
				     "memfd-unaligned"));
	EXPECT_TRUE(splice_and_check(mapped, 2 * PROCESS_PAGE_SIZE,
				     2 * PROCESS_PAGE_SIZE, "memfd-crossing"));
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
	close(fd);
}

TEST(vmsplice_unmapped_head_and_live_pipe_refault)
{
	unsigned char expected[PROCESS_PAGE_SIZE], readback[PROCESS_PAGE_SIZE];
	unsigned char rewritten[PROCESS_PAGE_SIZE];
	unsigned char *reservation, *base;
	struct iovec iov;
	unsigned long i;
	int pipefd[2];
	ssize_t ret;

	base = map_aligned(NATIVE_PAGE_SIZE, &reservation);
	ASSERT_NE(MAP_FAILED, base);
	for (i = 0; i < NATIVE_PAGE_SIZE; i++) {
		unsigned char value = (unsigned char)(0x30 +
						      (i / PROCESS_PAGE_SIZE) * 19 +
						      i);

		base[i] = value;
		if (i >= PROCESS_PAGE_SIZE && i < 2 * PROCESS_PAGE_SIZE)
			expected[i - PROCESS_PAGE_SIZE] = value;
	}

	ASSERT_EQ(0, munmap(base, PROCESS_PAGE_SIZE));
	ASSERT_EQ(0, pipe(pipefd));
	iov.iov_base = base + PROCESS_PAGE_SIZE;
	iov.iov_len = PROCESS_PAGE_SIZE;
	ret = vmsplice(pipefd[1], &iov, 1, 0);
	ASSERT_EQ((ssize_t)PROCESS_PAGE_SIZE, ret);

	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	memset(base + PROCESS_PAGE_SIZE, 0xa5, PROCESS_PAGE_SIZE);
	memset(rewritten, 0xa5, sizeof(rewritten));

	EXPECT_TRUE(read_full(pipefd[0], readback, PROCESS_PAGE_SIZE));
	EXPECT_EQ(0, memcmp(readback, expected, PROCESS_PAGE_SIZE));
	EXPECT_EQ(0, memcmp(base + PROCESS_PAGE_SIZE, rewritten,
			    PROCESS_PAGE_SIZE));

	close(pipefd[0]);
	close(pipefd[1]);
	munmap(reservation, 2 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
