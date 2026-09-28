// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) userfaultfd handler write-protects one 4K page of a compat
 * owner, receives the owner's WP fault at the 4K address, and lifts the
 * protection so the owner's write completes with the requested contents.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define RESERVE_SIZE		(3 * PROCESS_PAGE_SIZE)
#define OWNER_RESERVE_ADDRESS	((1UL << 28) + 3 * PROCESS_PAGE_SIZE)
#define INITIAL_VALUE		0x5a
#define WRITTEN_VALUE		0xa5

struct wp_setup_report {
	long page_size;
	int result;
	int error;
};

struct wp_fault_report {
	long poll_result;
	ssize_t read_result;
	__u64 event;
	__u64 flags;
	unsigned long address;
	int unprotect_result;
	int unprotect_error;
};

static int run_native_handler(int uffd, unsigned long destination,
			      int report_fd)
{
	struct wp_setup_report setup = {
		.page_size = sysconf(_SC_PAGESIZE),
		.result = -1,
	};
	struct wp_fault_report fault = {
		.poll_result = -1,
		.read_result = -1,
		.unprotect_result = -1,
	};
	struct uffdio_writeprotect wp = {
		.range.start = destination,
		.range.len = PROCESS_PAGE_SIZE,
		.mode = UFFDIO_WRITEPROTECT_MODE_WP,
	};
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	struct uffd_msg message = {};

	errno = 0;
	setup.result = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
	setup.error = errno;
	if (!write_full(report_fd, &setup, sizeof(setup)))
		return EXIT_FAILURE;

	if (!setup.result) {
		fault.poll_result = poll(&pollfd, 1, 1000);
		if (fault.poll_result > 0 && pollfd.revents & POLLIN) {
			fault.read_result = read(uffd, &message, sizeof(message));
			if (fault.read_result == sizeof(message)) {
				fault.event = message.event;
				fault.flags = message.arg.pagefault.flags;
				fault.address = message.arg.pagefault.address;
			}
		}
		wp.mode = 0;
		errno = 0;
		fault.unprotect_result = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
		fault.unprotect_error = errno;
	}

	if (!write_full(report_fd, &fault, sizeof(fault)))
		return EXIT_FAILURE;
	return setup.result == 0 && fault.poll_result > 0 &&
	       fault.read_result == sizeof(message) &&
	       fault.event == UFFD_EVENT_PAGEFAULT &&
	       fault.flags & UFFD_PAGEFAULT_FLAG_WP &&
	       fault.address == destination && !fault.unprotect_result ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_native_handler(int uffd, unsigned long destination,
			       int report_fd)
{
	char destination_arg[32];
	char report_fd_arg[16];
	char uffd_arg[16];

	snprintf(uffd_arg, sizeof(uffd_arg), "%d", uffd);
	snprintf(destination_arg, sizeof(destination_arg), "%lu", destination);
	snprintf(report_fd_arg, sizeof(report_fd_arg), "%d", report_fd);
	p3s_execl(false, NULL, "--handler", uffd_arg, destination_arg,
		   report_fd_arg, NULL);
	return EXIT_FAILURE;
}

FIXTURE(uffd_owner) {
	unsigned char *reservation;
	unsigned char *destination;
	int uffd;
};

FIXTURE_SETUP(uffd_owner)
{
	struct uffdio_register registration = {};
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP,
	};
	self->reservation = mmap((void *)OWNER_RESERVE_ADDRESS, RESERVE_SIZE,
				 PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS |
				 MAP_FIXED_NOREPLACE, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	self->destination = mmap(self->reservation + PROCESS_PAGE_SIZE,
				 PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_NE(self->destination, MAP_FAILED);
	self->uffd = syscall(SYS_userfaultfd, O_NONBLOCK);
	if (self->uffd < 0 && errno == EPERM)
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_GE(self->uffd, 0);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	ASSERT_TRUE(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP);
	registration.range.start = (unsigned long)self->destination;
	registration.range.len = PROCESS_PAGE_SIZE;
	registration.mode = UFFDIO_REGISTER_MODE_WP;
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_REGISTER, &registration));
	ASSERT_TRUE(registration.ioctls & (1ULL << _UFFDIO_WRITEPROTECT));
	*self->destination = INITIAL_VALUE;
}

FIXTURE_TEARDOWN(uffd_owner)
{
	close(self->uffd);
	munmap(self->reservation, RESERVE_SIZE);
}

struct wp_outcome {
	struct wp_setup_report setup;
	struct wp_fault_report fault;
	bool setup_reported;
	bool fault_reported;
	bool write_completed;
	bool waited;
	int handler_status;
	unsigned char value;
};

static struct wp_outcome run_wp_handler(FIXTURE_DATA(uffd_owner) *self)
{
	struct wp_outcome outcome = {};
	int report_pipe[2];
	pid_t handler;

	if (pipe(report_pipe))
		return outcome;
	handler = fork();
	if (handler < 0) {
		close(report_pipe[0]);
		close(report_pipe[1]);
		return outcome;
	}
	if (!handler) {
		close(report_pipe[0]);
		_exit(exec_native_handler(self->uffd,
					  (unsigned long)self->destination,
					  report_pipe[1]));
	}
	close(report_pipe[1]);
	outcome.setup_reported = read_full(report_pipe[0], &outcome.setup,
					  sizeof(outcome.setup));
	if (outcome.setup_reported && !outcome.setup.result) {
		*self->destination = WRITTEN_VALUE;
		outcome.write_completed = true;
	}
	outcome.fault_reported = read_full(report_pipe[0], &outcome.fault,
					  sizeof(outcome.fault));
	close(report_pipe[0]);
	outcome.waited = waitpid(handler, &outcome.handler_status, 0) == handler;
	outcome.value = *self->destination;
	return outcome;
}

TEST_F(uffd_owner, handler_uses_native_page_size)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.setup_reported);
	EXPECT_EQ(NATIVE_PAGE_SIZE, result.setup.page_size);
}

TEST_F(uffd_owner, handler_writeprotects_owner_page)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.setup_reported);
	EXPECT_EQ(0, result.setup.result);
}

TEST_F(uffd_owner, handler_receives_pagefault_event)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.fault_reported);
	EXPECT_GT(result.fault.poll_result, 0);
	EXPECT_EQ(sizeof(struct uffd_msg), result.fault.read_result);
	EXPECT_EQ(UFFD_EVENT_PAGEFAULT, result.fault.event);
}

TEST_F(uffd_owner, fault_carries_wp_flag)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.fault_reported);
	EXPECT_TRUE(result.fault.flags & UFFD_PAGEFAULT_FLAG_WP);
}

TEST_F(uffd_owner, fault_address_is_owner_4k_page)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.fault_reported);
	EXPECT_EQ((unsigned long)self->destination, result.fault.address);
}

TEST_F(uffd_owner, handler_removes_write_protection)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.fault_reported);
	EXPECT_EQ(0, result.fault.unprotect_result);
}

TEST_F(uffd_owner, owner_write_resumes_with_requested_bytes)
{
	struct wp_outcome result = run_wp_handler(self);

	ASSERT_TRUE(result.write_completed);
	EXPECT_EQ(WRITTEN_VALUE, result.value);
}

TEST_F(uffd_owner, native_handler_exits_cleanly)
{
	struct wp_outcome result = run_wp_handler(self);

	EXPECT_TRUE(result.waited && WIFEXITED(result.handler_status) &&
		    WEXITSTATUS(result.handler_status) == EXIT_SUCCESS);
}

static int child_role(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--handler"))
		return run_native_handler(atoi(argv[2]), strtoul(argv[3], NULL, 10),
					  atoi(argv[4]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
