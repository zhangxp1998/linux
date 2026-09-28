// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) userfaultfd handler poisons one 4K page of a compat owner
 * with UFFDIO_POISON; the owner can unregister the range and a reader of the
 * poisoned page is killed with SIGBUS.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define RESERVE_SIZE		(3 * PROCESS_PAGE_SIZE)
#define OWNER_RESERVE_ADDRESS	((1UL << 28) + 15 * PROCESS_PAGE_SIZE)

struct handler_report {
	long page_size;
	int result;
	int error;
	__s64 updated;
};

static int run_native_handler(int uffd, unsigned long destination,
			      int report_fd)
{
	struct uffdio_poison poison = {
		.range.start = destination,
		.range.len = PROCESS_PAGE_SIZE,
	};
	struct handler_report report = {
		.page_size = sysconf(_SC_PAGESIZE),
		.result = -1,
	};

	errno = 0;
	report.result = ioctl(uffd, UFFDIO_POISON, &poison);
	report.error = errno;
	report.updated = poison.updated;
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;
	return !report.result && report.updated == PROCESS_PAGE_SIZE ?
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
	struct uffdio_register registration = {
		.range.len = PROCESS_PAGE_SIZE,
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_POISON,
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
	ASSERT_TRUE(api.features & UFFD_FEATURE_POISON);
	registration.range.start = (unsigned long)self->destination;
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_REGISTER, &registration));
	ASSERT_TRUE(registration.ioctls & (1ULL << _UFFDIO_POISON));
}

FIXTURE_TEARDOWN(uffd_owner)
{
	close(self->uffd);
	munmap(self->reservation, RESERVE_SIZE);
}

struct handler_outcome {
	struct handler_report report;
	bool reported;
	bool waited;
	int status;
};

static struct handler_outcome poison_in_native_handler(
					 FIXTURE_DATA(uffd_owner) *self)
{
	struct handler_outcome outcome = {};
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
	outcome.reported = read_full(report_pipe[0], &outcome.report,
					    sizeof(outcome.report));
	close(report_pipe[0]);
	outcome.waited = waitpid(handler, &outcome.status, 0) == handler;
	return outcome;
}

TEST_F(uffd_owner, handler_uses_native_page_size)
{
	struct handler_outcome outcome = poison_in_native_handler(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(NATIVE_PAGE_SIZE, outcome.report.page_size);
}

TEST_F(uffd_owner, native_handler_poisons_one_4k_page)
{
	struct handler_outcome outcome = poison_in_native_handler(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(0, outcome.report.result);
	EXPECT_EQ(PROCESS_PAGE_SIZE, outcome.report.updated);
	EXPECT_TRUE(outcome.waited && WIFEXITED(outcome.status) &&
		    WEXITSTATUS(outcome.status) == EXIT_SUCCESS);
}

TEST_F(uffd_owner, owner_unregisters_poisoned_range)
{
	struct handler_outcome outcome = poison_in_native_handler(self);
	struct uffdio_range range = {
		.start = (unsigned long)self->destination,
		.len = PROCESS_PAGE_SIZE,
	};

	ASSERT_TRUE(outcome.reported);
	ASSERT_EQ(0, outcome.report.result);
	EXPECT_EQ(0, ioctl(self->uffd, UFFDIO_UNREGISTER, &range));
}

TEST_F(uffd_owner, reader_gets_sigbus_after_unregister)
{
	struct handler_outcome outcome = poison_in_native_handler(self);
	struct uffdio_range range = {
		.start = (unsigned long)self->destination,
		.len = PROCESS_PAGE_SIZE,
	};
	int status;
	pid_t reader;

	ASSERT_TRUE(outcome.reported);
	ASSERT_EQ(0, outcome.report.result);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_UNREGISTER, &range));
	reader = fork();
	ASSERT_GE(reader, 0);
	if (!reader)
		_exit(!!*self->destination);
	ASSERT_EQ(reader, waitpid(reader, &status, 0));
	EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS);
}

static int child_role(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--handler"))
		return run_native_handler(atoi(argv[2]), strtoul(argv[3], NULL, 10),
					  atoi(argv[4]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
