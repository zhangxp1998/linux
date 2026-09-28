// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat userfaultfd handler resolves a native (16K) owner's missing
 * faults with a one-native-page UFFDIO_COPY and UFFDIO_ZEROPAGE, and the
 * owner reads back all four source slices and the zeroed page.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

#define TEST_SIZE		(2 * NATIVE_PAGE_SIZE)
#define SOURCE_ADDRESS		(0x40000000UL + PROCESS_PAGE_SIZE)
#define TEST_VALUE		0x6b

struct handler_report {
	long page_size;
	long long copied;
	long long zeroed;
	int copy_result;
	int copy_error;
	int zero_result;
	int zero_error;
};

static bool buffer_is_value(const unsigned char *buffer, size_t length,
			    unsigned char value)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (buffer[i] != value)
			return false;
	}
	return true;
}

static int run_compat_handler(int uffd, unsigned long destination,
			      int report_fd)
{
	struct handler_report report = {
		.page_size = sysconf(_SC_PAGESIZE),
		.copy_result = -1,
		.zero_result = -1,
	};
	struct uffdio_copy copy = {
		.dst = destination,
		.len = NATIVE_PAGE_SIZE,
	};
	struct uffdio_zeropage zero = {
		.range = {
			.start = destination + NATIVE_PAGE_SIZE,
			.len = NATIVE_PAGE_SIZE,
		},
	};
	unsigned char *source;

	source = mmap((void *)SOURCE_ADDRESS, NATIVE_PAGE_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (source != MAP_FAILED) {
		memset(source, TEST_VALUE, NATIVE_PAGE_SIZE);
		copy.src = (unsigned long)source;
		report.copy_result = ioctl(uffd, UFFDIO_COPY, &copy);
		report.copy_error = errno;
		report.copied = copy.copy;
		if (!report.copy_result) {
			report.zero_result = ioctl(uffd, UFFDIO_ZEROPAGE, &zero);
			report.zero_error = errno;
			report.zeroed = zero.zeropage;
		}
		munmap(source, NATIVE_PAGE_SIZE);
	} else {
		report.copy_error = errno;
	}
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;
	return !report.copy_result && report.copied == NATIVE_PAGE_SIZE &&
	       !report.zero_result && report.zeroed == NATIVE_PAGE_SIZE ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_compat_handler(int uffd, unsigned long destination,
			       int report_fd)
{
	char destination_arg[32];
	char report_fd_arg[16];
	char uffd_arg[16];

	snprintf(uffd_arg, sizeof(uffd_arg), "%d", uffd);
	snprintf(destination_arg, sizeof(destination_arg), "%lu", destination);
	snprintf(report_fd_arg, sizeof(report_fd_arg), "%d", report_fd);
	p3s_execl(true, NULL, "--handler", uffd_arg, destination_arg,
		   report_fd_arg, NULL);
	return EXIT_FAILURE;
}

FIXTURE(native_owner) {
	unsigned char *destination;
	int uffd;
};

FIXTURE_SETUP(native_owner)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
		.range.len = TEST_SIZE,
	};
	struct uffdio_api api = { .api = UFFD_API };

	ASSERT_EQ(NATIVE_PAGE_SIZE, sysconf(_SC_PAGESIZE));
	self->destination = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->destination, MAP_FAILED);
	self->uffd = syscall(SYS_userfaultfd,
			     O_NONBLOCK | UFFD_USER_MODE_ONLY);
	if (self->uffd < 0 && (errno == EPERM || errno == EOPNOTSUPP))
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_GE(self->uffd, 0);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	registration.range.start = (unsigned long)self->destination;
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_REGISTER, &registration));
}

FIXTURE_TEARDOWN(native_owner)
{
	close(self->uffd);
	munmap(self->destination, TEST_SIZE);
}

struct owner_outcome {
	struct handler_report report;
	bool reported;
	bool waited;
	int handler_status;
	bool mincore_ok;
	bool copy_resident;
	bool zero_resident;
	bool copy_contents_ok;
	bool zero_contents_ok;
};

static struct owner_outcome run_compat_copy(FIXTURE_DATA(native_owner) *self)
{
	struct owner_outcome outcome = {};
	unsigned char residency[2] = {};
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
		_exit(exec_compat_handler(self->uffd,
					  (unsigned long)self->destination,
					  report_pipe[1]));
	}
	close(report_pipe[1]);
	outcome.reported = read_full(report_pipe[0], &outcome.report,
					    sizeof(outcome.report));
	close(report_pipe[0]);
	outcome.waited = waitpid(handler, &outcome.handler_status, 0) == handler;
	outcome.mincore_ok = !mincore(self->destination, TEST_SIZE, residency);
	if (outcome.mincore_ok) {
		outcome.copy_resident = residency[0] & 1;
		outcome.zero_resident = residency[1] & 1;
	}
	if (outcome.copy_resident)
		outcome.copy_contents_ok = buffer_is_value(self->destination,
							  NATIVE_PAGE_SIZE, TEST_VALUE);
	if (outcome.zero_resident)
		outcome.zero_contents_ok = buffer_is_value(self->destination +
						   NATIVE_PAGE_SIZE,
						   NATIVE_PAGE_SIZE, 0);
	return outcome;
}

TEST_F(native_owner, handler_uses_4k_page_size)
{
	struct owner_outcome outcome = run_compat_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(PROCESS_PAGE_SIZE, outcome.report.page_size);
}

TEST_F(native_owner, compat_handler_copies_one_native_page)
{
	struct owner_outcome outcome = run_compat_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(0, outcome.report.copy_result);
	EXPECT_EQ(NATIVE_PAGE_SIZE, outcome.report.copied);
}

TEST_F(native_owner, copy_populates_native_owner_page)
{
	struct owner_outcome outcome = run_compat_copy(self);

	EXPECT_TRUE(outcome.mincore_ok && outcome.copy_resident);
}

TEST_F(native_owner, native_owner_reads_four_source_slices)
{
	struct owner_outcome outcome = run_compat_copy(self);

	EXPECT_TRUE(outcome.copy_contents_ok);
}

TEST_F(native_owner, compat_handler_zeroes_one_native_page)
{
	struct owner_outcome outcome = run_compat_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(0, outcome.report.zero_result);
	EXPECT_EQ(NATIVE_PAGE_SIZE, outcome.report.zeroed);
}

TEST_F(native_owner, native_owner_reads_zeroed_page)
{
	struct owner_outcome outcome = run_compat_copy(self);

	EXPECT_TRUE(outcome.mincore_ok && outcome.zero_resident &&
		    outcome.zero_contents_ok);
}

TEST_F(native_owner, compat_handler_exits_cleanly)
{
	struct owner_outcome outcome = run_compat_copy(self);

	EXPECT_TRUE(outcome.waited && WIFEXITED(outcome.handler_status) &&
		    WEXITSTATUS(outcome.handler_status) == EXIT_SUCCESS);
}

static int child_role(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--handler"))
		return run_compat_handler(atoi(argv[2]),
					  strtoul(argv[3], NULL, 10),
					  atoi(argv[4]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
