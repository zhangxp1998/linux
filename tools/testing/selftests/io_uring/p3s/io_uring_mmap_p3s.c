// SPDX-License-Identifier: GPL-2.0
/* Reject io_uring from 4K compat processes, including a transferred ring. */
#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

static int ring_fd;

static int setup_ring(void)
{
	struct io_uring_params params = {};

	return syscall(SYS_io_uring_setup, 2, &params);
}

TEST(compat_setup_is_rejected)
{
	struct io_uring_params params = {};
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_setup, 2, &params);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(compat_enter_on_transferred_ring_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_enter, ring_fd, 0, 0, 0, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(compat_register_on_transferred_ring_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_register, ring_fd,
		      IORING_UNREGISTER_BUFFERS, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(compat_blind_registration_is_rejected)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_io_uring_register, -1, IORING_REGISTER_BUFFERS, NULL, 0);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
}

TEST(compat_mmap_on_transferred_ring_is_rejected)
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

int main(int argc, char **argv)
{
	char fd_arg[32];
	int fd;

	p3s_argv0 = argv[0];
	if (argc == 3 && !strcmp(argv[1], "--compat")) {
		int result;

		p3s_require_compat();
		ring_fd = atoi(argv[2]);
		result = p3s_harness_run();
		close(ring_fd);
		return result;
	}
	if (argc != 1)
		return EXIT_FAILURE;
#ifndef __aarch64__
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / 4K compat geometry\n");
#else
	if (getpagesize() != NATIVE_PAGE_SIZE) {
		ksft_print_header();
		ksft_exit_skip("native exec does not use 16K pages\n");
	}
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
	exec_compat(argv[0], "--compat", fd_arg, NULL);
#endif
}
