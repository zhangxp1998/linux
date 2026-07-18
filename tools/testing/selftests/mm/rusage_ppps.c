// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define PARENT_SIZE	(32UL * 1024 * 1024)
#define CHILD_4K_SIZE	(24UL * 1024 * 1024)
#define CHILD_NATIVE_SIZE (40UL * 1024 * 1024)

struct child_report {
	long page_size;
	unsigned long hwm_kb;
};

static bool read_hwm(unsigned long *hwm_kb)
{
	char status[8192];
	char *line;
	ssize_t length;
	int fd;

	fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	length = read(fd, status, sizeof(status) - 1);
	close(fd);
	if (length < 0)
		return false;
	status[length] = '\0';
	line = strstr(status, "VmHWM:");
	return line && sscanf(line, "VmHWM: %lu kB", hwm_kb) == 1;
}

static void *fault_mapping(size_t length)
{
	unsigned char *mapping;
	size_t offset;

	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	for (offset = 0; offset < length; offset += USER_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset + 1;
	return mapping;
}

static bool close_enough(unsigned long actual, unsigned long expected)
{
	unsigned long difference = actual > expected ? actual - expected :
		expected - actual;

	return difference <= 512;
}

static void child_workload(int report_fd, size_t length)
{
	struct child_report report;
	void *mapping;

	mapping = fault_mapping(length);
	if (mapping == MAP_FAILED || !read_hwm(&report.hwm_kb))
		_exit(120);
	report.page_size = sysconf(_SC_PAGESIZE);
	if (write(report_fd, &report, sizeof(report)) != sizeof(report))
		_exit(121);
	close(report_fd);
	munmap(mapping, length);
	_exit(0);
}

static bool collect_child(bool native, size_t length,
			  struct child_report *report)
{
	char fd_string[24];
	char length_string[32];
	int pipefd[2];
	int status;
	pid_t pid;

	if (pipe(pipefd))
		return false;
	pid = fork();
	if (!pid) {
		close(pipefd[0]);
		if (!native)
			child_workload(pipefd[1], length);
		if (personality(personality(0xffffffffUL) &
				~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
			_exit(122);
		snprintf(fd_string, sizeof(fd_string), "%d", pipefd[1]);
		snprintf(length_string, sizeof(length_string), "%zu", length);
		execl("/proc/self/exe", "rusage_ppps", "--native-child",
		      fd_string, length_string, NULL);
		_exit(123);
	}
	close(pipefd[1]);
	if (pid < 0 || read(pipefd[0], report, sizeof(*report)) !=
						       sizeof(*report)) {
		close(pipefd[0]);
		return false;
	}
	close(pipefd[0]);
	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0;
}

static int run_test(void)
{
	struct child_report child_4k = { };
	struct child_report child_native = { };
	struct rusage usage;
	unsigned long expected_children;
	unsigned long parent_hwm = 0;
	void *mapping;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "parent process uses 4K pages\n");

	mapping = fault_mapping(PARENT_SIZE);
	ksft_test_result(mapping != MAP_FAILED && read_hwm(&parent_hwm),
			 "fault and measure the parent workload\n");
	if (mapping == MAP_FAILED || !parent_hwm)
		ksft_exit_fail_msg("parent workload setup failed: %s\n",
				   strerror(errno));
	ksft_test_result(!getrusage(RUSAGE_SELF, &usage),
			 "read RUSAGE_SELF\n");
	ksft_print_msg("self VmHWM=%lu ru_maxrss=%ld\n", parent_hwm,
		       usage.ru_maxrss);
	ksft_test_result(close_enough(usage.ru_maxrss, parent_hwm),
			 "report self maxrss in process-page KB\n");
	munmap(mapping, PARENT_SIZE);

	ksft_test_result(collect_child(false, CHILD_4K_SIZE, &child_4k),
			 "collect a 4K child's high-watermark\n");
	ksft_test_result(collect_child(true, CHILD_NATIVE_SIZE, &child_native),
			 "collect a native-page child's high-watermark\n");
	ksft_print_msg("child4k page=%ld hwm=%lu child_native page=%ld hwm=%lu\n",
		       child_4k.page_size, child_4k.hwm_kb,
		       child_native.page_size, child_native.hwm_kb);
	ksft_test_result(!getrusage(RUSAGE_CHILDREN, &usage),
			 "read RUSAGE_CHILDREN\n");
	expected_children = child_4k.hwm_kb > child_native.hwm_kb ?
		child_4k.hwm_kb : child_native.hwm_kb;
	ksft_print_msg("children expected_max=%lu ru_maxrss=%ld\n",
		       expected_children, usage.ru_maxrss);
	ksft_test_result(close_enough(usage.ru_maxrss, expected_children),
			 "compare child maxrss values in common KB units\n");

	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "rusage_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	if (argc == 4 && !strcmp(argv[1], "--native-child"))
		child_workload(atoi(argv[2]), strtoull(argv[3], NULL, 0));
	return EXIT_FAILURE;
}
