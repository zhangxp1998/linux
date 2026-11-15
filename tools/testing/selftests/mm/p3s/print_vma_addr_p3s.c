// SPDX-License-Identifier: GPL-2.0
/*
 * An unhandled exception trace for a 4K compat process reports the mapping's
 * 4K file offset in print_vma_addr(), checked by executing UDF from a private
 * memfd mapping at file offset 4K and reading the kernel log.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#ifndef SYSLOG_ACTION_READ_ALL
#define SYSLOG_ACTION_READ_ALL 3
#endif

#ifndef SYSLOG_ACTION_SIZE_BUFFER
#define SYSLOG_ACTION_SIZE_BUFFER 10
#endif

#define TEST_COMM "vmaoff-p3s"
#define EXCEPTION_TRACE_PATH "/proc/sys/debug/exception-trace"

FIXTURE(exception_trace) {
	int original;
	bool restore;
};

FIXTURE_SETUP(exception_trace)
{
}

static bool write_exception_trace(int value)
{
	char text[4];
	int fd, len;
	bool success;

	len = snprintf(text, sizeof(text), "%d\n", value);
	fd = open(EXCEPTION_TRACE_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	success = write(fd, text, len) == len;
	return close(fd) == 0 && success;
}

FIXTURE_TEARDOWN_PARENT(exception_trace)
{
	if (self->restore)
		EXPECT_TRUE(write_exception_trace(self->original));
}

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

static char *last_exception_message(char *log, pid_t pid)
{
	char marker[64];
	char *message = NULL;
	char *next = log;

	snprintf(marker, sizeof(marker), "%s[%d]: unhandled exception:",
		 TEST_COMM, (int)pid);
	while ((next = strstr(next, marker))) {
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

TEST_F(exception_trace, p3s_print_vma_addr)
{
	const uint32_t udf_instruction = 0;
	void (*trigger)(void);
	char *message;
	char *log;
	char *newline;
	void *mapping;
	pid_t child;
	int status;
	int memfd;
	FILE *trace;

	trace = fopen(EXCEPTION_TRACE_PATH, "re");
	if (!trace)
		SKIP(return, "exception trace is unavailable: %s", strerror(errno));
	ASSERT_EQ(1, fscanf(trace, "%d", &self->original));
	fclose(trace);
	if (!write_exception_trace(1))
		SKIP(return, "cannot enable exception trace: %s", strerror(errno));
	self->restore = true;

	memfd = memfd_create("print-vma-addr-p3s", MFD_CLOEXEC);
	ASSERT_GE(memfd, 0);
	ASSERT_EQ(0, ftruncate(memfd, 2 * PROCESS_PAGE_SIZE));
	ASSERT_EQ((ssize_t)sizeof(udf_instruction),
		  pwrite(memfd, &udf_instruction, sizeof(udf_instruction),
			 PROCESS_PAGE_SIZE));
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC,
		       MAP_PRIVATE, memfd, PROCESS_PAGE_SIZE);
	ASSERT_NE(mapping, MAP_FAILED);

	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		/* The kernel only logs signals that are unhandled. */
		signal(SIGILL, SIG_DFL);
		prctl(PR_SET_NAME, TEST_COMM, 0, 0, 0);
		trigger = mapping;
		trigger();
		_exit(EXIT_FAILURE);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);

	log = read_kernel_log();
	ASSERT_NE(log, NULL);
	message = last_exception_message(log, child);
	newline = message ? strchr(message, '\n') : NULL;
	if (newline)
		*newline = '\0';
	ksft_print_msg("exception trace: %s\n", message ?: "not found");
	EXPECT_TRUE(message && strstr(message, "[1000,"));

	free(log);
	munmap(mapping, PROCESS_PAGE_SIZE);
	close(memfd);
}

P3S_HARNESS_4KB_MAIN
