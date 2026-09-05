// SPDX-License-Identifier: GPL-2.0
/*
 * Fork must give a 4K compat child a private, still-packed copy when the
 * parent's anonymous tuple is pinned by an io_uring fixed-buffer registration.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define TUPLE_SLICES (NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE)
#define IORING_REGISTER_BUFFERS 0
#define IORING_UNREGISTER_BUFFERS 1

static int setup_ring(unsigned int entries)
{
	/* Large enough and naturally aligned for struct io_uring_params. */
	unsigned long params[32] = {};

	return syscall(SYS_io_uring_setup, entries, &params);
}

static int register_ring(int fd, unsigned int opcode, void *arg,
			 unsigned int nr_args)
{
	return syscall(SYS_io_uring_register, fd, opcode, arg, nr_args);
}

static bool tuple_has_markers(const unsigned char *tuple)
{
	unsigned int slice;

	for (slice = 0; slice < TUPLE_SLICES; slice++)
		if (tuple[slice * PROCESS_PAGE_SIZE] != 0x40 + slice)
			return false;
	return true;
}

static int child_check(unsigned char *tuple)
{
	unsigned long anonymous = 0;

	if (!tuple_has_markers(tuple) ||
	    !ppps_smaps_bytes(tuple, NATIVE_PAGE_SIZE, "Anonymous",
			      &anonymous) ||
	    anonymous != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	tuple[2 * PROCESS_PAGE_SIZE] = 0xa5;
	return tuple[2 * PROCESS_PAGE_SIZE] == 0xa5 ? EXIT_SUCCESS :
						      EXIT_FAILURE;
}

static int run_test(void)
{
	unsigned char *tuple;
	unsigned long anonymous = 0;
	struct iovec iov;
	unsigned int slice;
	int ring_fd;
	int registered;
	int status = 0;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(6);

	ring_fd = setup_ring(2);
	ksft_test_result(ring_fd >= 0, "create io_uring pin owner\n");
	if (ring_fd < 0)
		ksft_exit_fail_msg("io_uring_setup failed: %s\n",
				   strerror(errno));

	tuple = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (tuple == MAP_FAILED)
		ksft_exit_fail_msg("anonymous tuple mmap failed: %s\n",
				   strerror(errno));
	for (slice = 0; slice < TUPLE_SLICES; slice++)
		tuple[slice * PROCESS_PAGE_SIZE] = 0x40 + slice;
	ksft_test_result(tuple_has_markers(tuple) &&
			 ppps_smaps_bytes(tuple, NATIVE_PAGE_SIZE,
					  "Anonymous", &anonymous) &&
			 anonymous == NATIVE_PAGE_SIZE,
			 "populate one packed anonymous tuple\n");

	iov.iov_base = tuple;
	iov.iov_len = NATIVE_PAGE_SIZE;
	registered = register_ring(ring_fd, IORING_REGISTER_BUFFERS, &iov, 1);
	ksft_test_result(registered == 0,
			 "pin the complete tuple as a fixed buffer (ret=%d errno=%d)\n",
			 registered, registered < 0 ? errno : 0);
	if (registered)
		ksft_exit_fail_msg("fixed-buffer registration failed\n");

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child)
		_exit(child_check(tuple));
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
			 "child receives one private packed copy and can write it\n");
	ksft_test_result(tuple_has_markers(tuple),
			 "child write leaves the pinned parent tuple unchanged\n");

	registered = register_ring(ring_fd, IORING_UNREGISTER_BUFFERS, NULL, 0);
	ksft_test_result(registered == 0,
			 "unregister the fixed buffer (ret=%d errno=%d)\n",
			 registered, registered < 0 ? errno : 0);

	munmap(tuple, NATIVE_PAGE_SIZE);
	close(ring_fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
