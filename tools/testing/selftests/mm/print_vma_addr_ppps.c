// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef SYSLOG_ACTION_READ_ALL
#define SYSLOG_ACTION_READ_ALL 3
#endif

#ifndef SYSLOG_ACTION_SIZE_BUFFER
#define SYSLOG_ACTION_SIZE_BUFFER 10
#endif

#define USER_PAGE_SIZE 4096UL
#define TEST_COMM "vmaoff-ppps"

static char *read_kernel_log(void)
{
	char *log;
	int size;
	int len;

	size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
	if (size < 0)
		return NULL;
	log = malloc(size + 1);
	if (!log)
		return NULL;
	len = klogctl(SYSLOG_ACTION_READ_ALL, log, size);
	if (len < 0) {
		free(log);
		return NULL;
	}
	log[len] = '\0';
	return log;
}

static char *last_exception_message(char *log)
{
	char *message = NULL;
	char *next = log;

	while ((next = strstr(next, TEST_COMM))) {
		char *exception = strstr(next, "unhandled exception:");
		char *newline = strchr(next, '\n');

		if (exception && (!newline || exception < newline))
			message = next;
		if (!newline)
			break;
		next = newline + 1;
	}
	return message;
}

static int run_test(void)
{
	const uint32_t udf_instruction = 0;
	void (*trigger)(void);
	char *message;
	char *log;
	char *newline;
	void *mapping;
	pid_t child;
	int status;
	int trace_fd;
	int memfd;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	trace_fd = open("/proc/sys/debug/exception-trace", O_WRONLY | O_CLOEXEC);
	if (trace_fd < 0 || write(trace_fd, "1\n", 2) != 2)
		ksft_exit_skip("cannot enable exception trace: %s\n",
			       strerror(errno));
	close(trace_fd);

	memfd = memfd_create("print-vma-addr-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, 2 * USER_PAGE_SIZE) ||
	    pwrite(memfd, &udf_instruction, sizeof(udf_instruction),
		   USER_PAGE_SIZE) != (ssize_t)sizeof(udf_instruction))
		ksft_exit_fail_msg("create executable memfd failed: %s\n",
				   strerror(errno));
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE,
		       memfd, USER_PAGE_SIZE);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		prctl(PR_SET_NAME, TEST_COMM, 0, 0, 0);
		trigger = mapping;
		trigger();
		_exit(EXIT_FAILURE);
	}
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL,
			 "execute UDF from the file mapping\n");

	log = read_kernel_log();
	if (!log)
		ksft_exit_fail_msg("read kernel log failed: %s\n",
				   strerror(errno));
	message = last_exception_message(log);
	newline = message ? strchr(message, '\n') : NULL;
	if (newline)
		*newline = '\0';
	ksft_print_msg("exception trace: %s\n", message ?: "not found");
	ksft_test_result(message && strstr(message, "[1000,"),
			 "report the 4K file offset in print_vma_addr()\n");

	free(log);
	munmap(mapping, USER_PAGE_SIZE);
	close(memfd);
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
	execl("/proc/self/exe", "print_vma_addr_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
