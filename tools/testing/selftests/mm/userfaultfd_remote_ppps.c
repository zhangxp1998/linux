// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define RESERVE_SIZE	(3 * USER_PAGE_SIZE)
#define NATIVE_SOURCE_ADDRESS	(1UL << 40)
#define TEST_VALUE	0x5a

struct handler_report {
	long page_size;
	long long copied;
	int result;
	int error;
};

static bool write_full(int fd, const void *buffer, size_t size)
{
	const char *position = buffer;

	while (size) {
		ssize_t written = write(fd, position, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		position += written;
		size -= written;
	}
	return true;
}

static bool read_full(int fd, void *buffer, size_t size)
{
	char *position = buffer;

	while (size) {
		ssize_t bytes = read(fd, position, size);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		size -= bytes;
	}
	return true;
}

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
	};
	struct uffdio_copy copy = {
		.dst = destination,
		.len = USER_PAGE_SIZE,
	};
	int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
	void *mmap_address = NULL;
	unsigned char *source;

	if (report.page_size > (long)USER_PAGE_SIZE) {
		mmap_address = (void *)NATIVE_SOURCE_ADDRESS;
		mmap_flags |= MAP_FIXED_NOREPLACE;
	}
	source = mmap(mmap_address, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      mmap_flags, -1, 0);
	if (source != MAP_FAILED) {
		memset(source, TEST_VALUE, USER_PAGE_SIZE);
		copy.src = (unsigned long)source;
		report.result = ioctl(uffd, UFFDIO_COPY, &copy);
		report.error = errno;
		report.copied = copy.copy;
		munmap(source, USER_PAGE_SIZE);
	} else {
		report.result = -1;
		report.error = errno;
	}
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;
	return report.result == 0 && report.copied == USER_PAGE_SIZE ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_native_handler(int uffd, unsigned long destination,
			       int report_fd)
{
	char destination_arg[32];
	char report_fd_arg[16];
	char uffd_arg[16];
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	snprintf(uffd_arg, sizeof(uffd_arg), "%d", uffd);
	snprintf(destination_arg, sizeof(destination_arg), "%lu", destination);
	snprintf(report_fd_arg, sizeof(report_fd_arg), "%d", report_fd);
	execl("/proc/self/exe", "userfaultfd_remote_ppps", "--handler",
	      uffd_arg, destination_arg, report_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_compat_owner(void)
{
	struct uffdio_register registration = {};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	struct handler_report report = {};
	unsigned char *destination;
	unsigned char *reservation;
	unsigned char residency = 0;
	int report_pipe[2];
	int handler_status = 0;
	bool handler_reported;
	bool setup_ok;
	bool resident;
	bool contents_ok = false;
	pid_t handler;
	int uffd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "owner process uses 4K pages\n");

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	destination = reservation == MAP_FAILED ? MAP_FAILED :
		mmap(reservation + USER_PAGE_SIZE, USER_PAGE_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	uffd = syscall(SYS_userfaultfd, O_NONBLOCK);
	setup_ok = destination != MAP_FAILED && uffd >= 0 &&
		   !ioctl(uffd, UFFDIO_API, &api);
	if (setup_ok) {
		registration.range.start = (unsigned long)destination;
		registration.range.len = USER_PAGE_SIZE;
		registration.mode = UFFDIO_REGISTER_MODE_MISSING;
		setup_ok = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	}
	ksft_test_result(setup_ok, "register an isolated missing 4K range\n");
	if (!setup_ok) {
		if (uffd < 0 && errno == EPERM)
			ksft_exit_skip("userfaultfd unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd setup failed: %s\n",
				   strerror(errno));
	}
	if (pipe(report_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	handler = fork();
	if (handler < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!handler) {
		close(report_pipe[0]);
		_exit(exec_native_handler(uffd, (unsigned long)destination,
					  report_pipe[1]));
	}

	close(report_pipe[1]);
	handler_reported = read_full(report_pipe[0], &report, sizeof(report));
	close(report_pipe[0]);
	waitpid(handler, &handler_status, 0);
	ksft_test_result(handler_reported &&
			 (report.page_size == USER_PAGE_SIZE ||
			  report.page_size == 4 * USER_PAGE_SIZE),
			 "start a native userfaultfd handler\n");
	ksft_print_msg("owner page size=%ld, handler page size=%ld\n",
		       sysconf(_SC_PAGESIZE), report.page_size);
	ksft_test_result(handler_reported && report.result == 0 &&
			 report.copied == USER_PAGE_SIZE,
			 "native handler copies one 4K page (%lld, %s)\n",
			 report.copied,
			 report.result ? strerror(report.error) : "ok");

	resident = !mincore(destination, USER_PAGE_SIZE, &residency) &&
		   (residency & 1);
	ksft_test_result(resident,
			 "UFFDIO_COPY populates the owner address\n");
	if (resident)
		contents_ok = buffer_is_value(destination, USER_PAGE_SIZE,
					      TEST_VALUE);
	ksft_test_result(contents_ok && WIFEXITED(handler_status) &&
			 WEXITSTATUS(handler_status) == EXIT_SUCCESS,
			 "owner reads the copied 4K page contents\n");

	close(uffd);
	munmap(reservation, RESERVE_SIZE);
	ksft_finished();
}

static int exec_compat_owner(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	execl("/proc/self/exe", "userfaultfd_remote_ppps", "--owner", NULL);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat_owner();
	if (argc == 2 && !strcmp(argv[1], "--owner"))
		return run_compat_owner();
	if (argc == 5 && !strcmp(argv[1], "--handler"))
		return run_native_handler(atoi(argv[2]), strtoul(argv[3], NULL, 10),
					  atoi(argv[4]));
	return EXIT_FAILURE;
}
