// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) userfaultfd handler resolves one 4K page of a compat owner
 * with UFFDIO_COPY from a high untagged source and UFFDIO_ZEROPAGE, and the owner
 * sees exactly those 4K pages populated.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define TEST_SIZE	(2 * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(TEST_SIZE + 2 * PROCESS_PAGE_SIZE)
#define OWNER_RESERVE_ADDRESS	((1UL << 28) + 3 * PROCESS_PAGE_SIZE)
#define NATIVE_SOURCE_ADDRESS	(1UL << 40)
#define TEST_VALUE	0x5a

struct handler_report {
	long page_size;
	long long copied;
	long long zeroed;
	int result;
	int error;
	int zero_result;
	int zero_error;
};

static bool buffer_is_value(const unsigned char *buffer, size_t size,
			    unsigned char value)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (buffer[i] != value)
			return false;
	}
	return true;
}

static int run_native_handler(int uffd, unsigned long destination,
			      int report_fd)
{
	struct handler_report report = {
		.page_size = sysconf(_SC_PAGESIZE),
		.result = -1,
		.zero_result = -1,
	};
	struct uffdio_copy copy = {
		.dst = destination,
		.len = PROCESS_PAGE_SIZE,
	};
	struct uffdio_zeropage zeropage = {
		.range.start = destination + PROCESS_PAGE_SIZE,
		.range.len = PROCESS_PAGE_SIZE,
	};
	int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
	void *mmap_address = NULL;
	unsigned char *source;

	if (report.page_size > (long)PROCESS_PAGE_SIZE) {
		mmap_address = (void *)NATIVE_SOURCE_ADDRESS;
		mmap_flags |= MAP_FIXED_NOREPLACE;
	}
	source = mmap(mmap_address, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      mmap_flags, -1, 0);
	if (source != MAP_FAILED) {
		memset(source, TEST_VALUE, PROCESS_PAGE_SIZE);
		copy.src = (unsigned long)source;
		report.result = ioctl(uffd, UFFDIO_COPY, &copy);
		report.error = errno;
		report.copied = copy.copy;
		if (!report.result) {
			report.zero_result = ioctl(uffd, UFFDIO_ZEROPAGE,
						   &zeropage);
			report.zero_error = errno;
			report.zeroed = zeropage.zeropage;
		}
		munmap(source, PROCESS_PAGE_SIZE);
	} else {
		report.result = -1;
		report.error = errno;
	}
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;
	return report.result == 0 && report.copied == PROCESS_PAGE_SIZE &&
	       report.zero_result == 0 && report.zeroed == PROCESS_PAGE_SIZE ?
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
		.range.len = TEST_SIZE,
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = { .api = UFFD_API };

	self->reservation = mmap((void *)OWNER_RESERVE_ADDRESS, RESERVE_SIZE,
				 PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS |
				 MAP_FIXED_NOREPLACE, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	self->destination = mmap(self->reservation + PROCESS_PAGE_SIZE,
				 TEST_SIZE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_NE(self->destination, MAP_FAILED);
	self->uffd = syscall(SYS_userfaultfd, O_NONBLOCK);
	if (self->uffd < 0 && errno == EPERM)
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_GE(self->uffd, 0);
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_API, &api));
	registration.range.start = (unsigned long)self->destination;
	ASSERT_EQ(0, ioctl(self->uffd, UFFDIO_REGISTER, &registration));
}

FIXTURE_TEARDOWN(uffd_owner)
{
	close(self->uffd);
	munmap(self->reservation, RESERVE_SIZE);
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

static struct owner_outcome run_native_copy(FIXTURE_DATA(uffd_owner) *self)
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
		_exit(exec_native_handler(self->uffd,
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
							  PROCESS_PAGE_SIZE, TEST_VALUE);
	if (outcome.zero_resident)
		outcome.zero_contents_ok = buffer_is_value(self->destination +
						   PROCESS_PAGE_SIZE,
						   PROCESS_PAGE_SIZE, 0);
	return outcome;
}

TEST_F(uffd_owner, handler_starts_with_supported_page_size)
{
	struct owner_outcome outcome = run_native_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(NATIVE_PAGE_SIZE, outcome.report.page_size);
}

TEST_F(uffd_owner, handler_copies_one_4k_page_from_high_source)
{
	struct owner_outcome outcome = run_native_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(0, outcome.report.result);
	EXPECT_EQ(PROCESS_PAGE_SIZE, outcome.report.copied);
}

TEST_F(uffd_owner, copy_populates_owner_address)
{
	struct owner_outcome outcome = run_native_copy(self);

	EXPECT_TRUE(outcome.mincore_ok && outcome.copy_resident);
}

TEST_F(uffd_owner, owner_reads_copied_4k_contents)
{
	struct owner_outcome outcome = run_native_copy(self);

	EXPECT_TRUE(outcome.copy_contents_ok);
}

TEST_F(uffd_owner, handler_zeroes_one_4k_page)
{
	struct owner_outcome outcome = run_native_copy(self);

	ASSERT_TRUE(outcome.reported);
	EXPECT_EQ(0, outcome.report.zero_result);
	EXPECT_EQ(PROCESS_PAGE_SIZE, outcome.report.zeroed);
}

TEST_F(uffd_owner, zero_populates_owner_address)
{
	struct owner_outcome outcome = run_native_copy(self);

	EXPECT_TRUE(outcome.mincore_ok && outcome.zero_resident);
}

TEST_F(uffd_owner, owner_reads_zeroed_4k_contents)
{
	struct owner_outcome outcome = run_native_copy(self);

	EXPECT_TRUE(outcome.zero_contents_ok);
}

TEST_F(uffd_owner, native_handler_exits_cleanly)
{
	struct owner_outcome outcome = run_native_copy(self);

	EXPECT_TRUE(outcome.waited && WIFEXITED(outcome.handler_status) &&
		    WEXITSTATUS(outcome.handler_status) == EXIT_SUCCESS);
}

static int child_role(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--handler"))
		return run_native_handler(atoi(argv[2]), strtoul(argv[3], NULL, 10),
					  atoi(argv[4]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
