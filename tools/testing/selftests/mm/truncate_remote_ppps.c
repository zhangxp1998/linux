// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_OFFSET	(3 * USER_PAGE_SIZE)
#define MAPPING_SIZE	(2 * USER_PAGE_SIZE)
#define FILE_SIZE	(8 * USER_PAGE_SIZE)
#define TRUNCATED_SIZE	(4 * USER_PAGE_SIZE)

struct child_report {
	long page_size;
	bool mapped;
	bool initial_contents;
	bool below_eof_preserved;
	bool eof_faulted;
};

static sigjmp_buf fault_env;

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

static unsigned char page_pattern(unsigned int page)
{
	return 0x41 + page;
}

static bool initialize_file(int fd)
{
	unsigned char page[USER_PAGE_SIZE];
	unsigned int i;

	for (i = 0; i < FILE_SIZE / USER_PAGE_SIZE; i++) {
		ssize_t written;

		memset(page, page_pattern(i), sizeof(page));
		written = pwrite(fd, page, sizeof(page), i * USER_PAGE_SIZE);
		if (written != sizeof(page))
			return false;
	}
	return true;
}

static void sigbus_handler(int signal_number)
{
	siglongjmp(fault_env, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *address;
	return true;
}

static int run_compat_child(int file_fd, int report_fd, int command_fd)
{
	struct sigaction action = {
		.sa_handler = sigbus_handler,
	};
	struct child_report report = {
		.page_size = sysconf(_SC_PAGESIZE),
	};
	unsigned char truncated_value = 0;
	unsigned char *mapping;
	char command;

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ, MAP_SHARED, file_fd,
		       MAPPING_OFFSET);
	report.mapped = mapping != MAP_FAILED;
	if (report.mapped)
		report.initial_contents = mapping[0] == page_pattern(3) &&
			mapping[USER_PAGE_SIZE] == page_pattern(4);
	if (!write_full(report_fd, &report, sizeof(report)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;

	if (report.mapped) {
		report.below_eof_preserved = mapping[0] == page_pattern(3);
		sigemptyset(&action.sa_mask);
		if (!sigaction(SIGBUS, &action, NULL))
			report.eof_faulted = !read_byte(&mapping[USER_PAGE_SIZE],
							&truncated_value);
		munmap(mapping, MAPPING_SIZE);
	}
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;
	return report.below_eof_preserved && report.eof_faulted ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_compat_child(int file_fd, int report_fd, int command_fd)
{
	char command_fd_arg[16];
	char file_fd_arg[16];
	char report_fd_arg[16];
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	snprintf(file_fd_arg, sizeof(file_fd_arg), "%d", file_fd);
	snprintf(report_fd_arg, sizeof(report_fd_arg), "%d", report_fd);
	snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
	execl("/proc/self/exe", "truncate_remote_ppps", "--child",
	      file_fd_arg, report_fd_arg, command_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_parent(void)
{
	struct child_report report = {};
	int command_pipe[2];
	int report_pipe[2];
	int child_status = 0;
	char command = 1;
	bool initial_report;
	bool final_report;
	bool truncated;
	pid_t child;
	int fd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE ||
			 sysconf(_SC_PAGESIZE) == 4 * USER_PAGE_SIZE,
			 "truncating process uses a supported page size\n");

	fd = memfd_create("truncate-remote-ppps", 0);
	if (fd < 0 || ftruncate(fd, FILE_SIZE) || !initialize_file(fd))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	if (pipe(report_pipe) || pipe(command_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(report_pipe[0]);
		close(command_pipe[1]);
		_exit(exec_compat_child(fd, report_pipe[1], command_pipe[0]));
	}

	close(report_pipe[1]);
	close(command_pipe[0]);
	initial_report = read_full(report_pipe[0], &report, sizeof(report));
	ksft_test_result(initial_report && report.page_size == USER_PAGE_SIZE,
			 "mapping process uses 4K pages\n");
	ksft_test_result(initial_report && report.mapped &&
			 report.initial_contents,
			 "remote sliced mapping exposes both file pages\n");

	truncated = !ftruncate(fd, TRUNCATED_SIZE);
	ksft_test_result(truncated,
			 "native process truncates at the page-cache boundary\n");
	if (!write_full(command_pipe[1], &command, sizeof(command)))
		ksft_print_msg("failed to release child: %s\n", strerror(errno));
	close(command_pipe[1]);
	final_report = read_full(report_pipe[0], &report, sizeof(report));
	close(report_pipe[0]);
	if (waitpid(child, &child_status, 0) < 0)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));

	ksft_test_result(final_report && report.below_eof_preserved,
			 "remote mapping preserves data below the new EOF\n");
	ksft_test_result(final_report && report.eof_faulted &&
			 WIFEXITED(child_status) &&
			 WEXITSTATUS(child_status) == EXIT_SUCCESS,
			 "remote mapping faults at the new EOF\n");
	close(fd);
	ksft_finished();
}

static int exec_native_parent(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	execl("/proc/self/exe", "truncate_remote_ppps", "--parent", NULL);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_native_parent();
	if (argc == 2 && !strcmp(argv[1], "--parent"))
		return run_parent();
	if (argc == 5 && !strcmp(argv[1], "--child"))
		return run_compat_child(atoi(argv[2]), atoi(argv[3]),
					atoi(argv[4]));
	return EXIT_FAILURE;
}
