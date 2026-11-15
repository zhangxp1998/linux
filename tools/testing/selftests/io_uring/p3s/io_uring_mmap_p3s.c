// SPDX-License-Identifier: GPL-2.0
/* Reject io_uring from 4KB processes, including a transferred ring. */
#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define P3S_HARNESS_LAUNCH prepare_harness
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

static int ring_fd;

static int setup_ring(void)
{
	struct io_uring_params params = {};

	return syscall(SYS_io_uring_setup, 2, &params);
}

TEST(four_kb_setup_is_rejected)
{
	struct io_uring_params params = {};
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_setup, 2, &params);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(four_kb_enter_on_transferred_ring_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_enter, ring_fd, 0, 0, 0, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(four_kb_register_on_transferred_ring_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_register, ring_fd,
		      IORING_UNREGISTER_BUFFERS, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(four_kb_blind_registration_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_register, -1, IORING_REGISTER_BUFFERS, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(four_kb_mmap_on_transferred_ring_is_rejected)
{
	void *mapping;

	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, ring_fd, IORING_OFF_SQ_RING);
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EOPNOTSUPP, errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

static int prepare_harness(int argc, char **argv)
{
	char fd_arg[32];
	int fd;

	if (p3s_harness_listing(argc, argv) || getenv("P3S_IO_URING_FD") ||
	    getpagesize() != NATIVE_PAGE_SIZE)
		return -1;
	fd = setup_ring();
	if (fd < 0) {
		ksft_print_header();
		ksft_exit_skip("native io_uring_setup unavailable: %s\n",
			       strerror(errno));
	}
	if (fcntl(fd, F_SETFD, 0))
		ksft_exit_fail_msg("clear ring FD_CLOEXEC failed: %s\n",
				   strerror(errno));
	snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
	if (setenv("P3S_IO_URING_FD", fd_arg, 1))
		ksft_exit_fail_msg("save ring FD failed: %s\n", strerror(errno));
	return -1;
}

static void close_ring(void)
{
	close(ring_fd);
}

static void setup_harness(int *argc, char ***argv)
{
	const char *saved = getenv("P3S_IO_URING_FD");

	if (!saved && p3s_harness_listing(*argc, *argv))
		return;
	if (!saved)
		ksft_exit_fail_msg("missing native io_uring FD\n");
	ring_fd = atoi(saved);
	atexit(close_ring);
}

P3S_HARNESS_4KB_MAIN
