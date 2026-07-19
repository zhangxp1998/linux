// SPDX-License-Identifier: GPL-2.0
/*
 * The oom:mark_victim trace record for a forced-OOM 4K compat victim reports
 * total-vm, file RSS and shmem RSS in process-page bytes matching its
 * /proc status, and anonymous RSS in native physical bytes.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define CHILD_MAP_SIZE (32UL * 1024 * 1024)
#define TRACE_ENABLE "/sys/kernel/tracing/events/oom/mark_victim/enable"
#define TRACE_FILE "/sys/kernel/tracing/trace"

struct victim_stats {
	long total_vm;
	long anon_rss;
	long file_rss;
	long shmem_rss;
};

static int write_text(const char *path, const char *text)
{
	ssize_t length = strlen(text);
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	written = write(fd, text, length);
	close(fd);
	return written == length ? 0 : -1;
}

static int clear_trace(void)
{
	int fd = open(TRACE_FILE, O_WRONLY | O_TRUNC | O_CLOEXEC);

	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

static long read_status_kb(pid_t pid, const char *name)
{
	char path[64];
	char line[256];
	FILE *file;
	long value = -1;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	file = fopen(path, "re");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, name, strlen(name)) &&
		    sscanf(line, "%*[^:]: %ld kB", &value) == 1)
			break;
		value = -1;
	}
	fclose(file);
	return value;
}

static char *read_trace(void)
{
	size_t capacity = 64 * 1024;
	size_t length = 0;
	char *buffer;
	int fd;

	fd = open(TRACE_FILE, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	buffer = malloc(capacity);
	if (!buffer) {
		close(fd);
		return NULL;
	}
	for (;;) {
		ssize_t bytes;

		if (length + 1 == capacity) {
			char *larger;

			capacity *= 2;
			larger = realloc(buffer, capacity);
			if (!larger) {
				free(buffer);
				close(fd);
				return NULL;
			}
			buffer = larger;
		}
		bytes = read(fd, buffer + length, capacity - length - 1);
		if (bytes < 0) {
			free(buffer);
			close(fd);
			return NULL;
		}
		if (!bytes)
			break;
		length += bytes;
	}
	close(fd);
	buffer[length] = '\0';
	return buffer;
}

static bool find_victim_stats(const char *trace, pid_t target,
			      struct victim_stats *stats)
{
	const char *record = trace;

	while ((record = strstr(record, "pid="))) {
		const char *memory;
		char comm[64];
		int pid;

		memory = strstr(record, "anon-rss=");
		if (sscanf(record, "pid=%d comm=%63s total-vm=%ldkB",
			   &pid, comm, &stats->total_vm) == 3 &&
		    memory &&
		    sscanf(memory, "anon-rss=%ldkB file-rss:%ldkB shmem-rss:%ldkB",
			   &stats->anon_rss, &stats->file_rss,
			   &stats->shmem_rss) == 3 && pid == target)
			return true;
		record += 4;
	}
	return false;
}

static void run_victim(int ready_fd)
{
	unsigned char *mapping;
	size_t offset;

	if (write_text("/proc/self/oom_score_adj", "1000"))
		_exit(2);
	mapping = mmap(NULL, CHILD_MAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		_exit(3);
	for (offset = 0; offset < CHILD_MAP_SIZE; offset += PROCESS_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset;
	if (write(ready_fd, "R", 1) != 1)
		_exit(4);
	close(ready_fd);
	for (;;)
		pause();
}

static bool within_tolerance(long actual, long expected)
{
	long tolerance = expected / 10;

	if (tolerance < 1024)
		tolerance = 1024;
	return labs(actual - expected) <= tolerance;
}

static int run_test(void)
{
	struct victim_stats trace_stats = { 0 };
	long expected_anon, expected_file, expected_shmem, expected_total;
	long expected_rss, traced_rss;
	char *trace = NULL;
	char ready = 0;
	bool child_ready;
	bool trace_ready;
	bool found = false;
	int pipefd[2];
	int status = 0;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(7);

	trace_ready = !write_text("/proc/self/oom_score_adj", "-1000") &&
		!write_text(TRACE_ENABLE, "1") && !clear_trace();
	ksft_test_result(trace_ready, "enable OOM victim tracing\n");
	if (!trace_ready)
		ksft_exit_fail_msg("trace setup failed: %s\n", strerror(errno));
	if (pipe(pipefd))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(pipefd[0]);
		run_victim(pipefd[1]);
	}
	close(pipefd[1]);
	child_ready = read(pipefd[0], &ready, 1) == 1 && ready == 'R';
	close(pipefd[0]);
	expected_total = read_status_kb(child, "VmSize");
	expected_anon = read_status_kb(child, "RssAnon");
	expected_file = read_status_kb(child, "RssFile");
	expected_shmem = read_status_kb(child, "RssShmem");
	child_ready = child_ready && expected_total > 0 && expected_anon > 0 &&
		expected_file >= 0 && expected_shmem >= 0;
	ksft_test_result(child_ready, "prepare a stable OOM victim\n");

	errno = 0;
	if (write_text("/proc/sysrq-trigger", "f"))
		kill(child, SIGKILL);
	waitpid(child, &status, 0);
	ksft_test_result(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
			 "force OOM to kill the selected child\n");

	trace = read_trace();
	if (trace)
		found = find_victim_stats(trace, child, &trace_stats);
	ksft_test_result(found, "capture the OOM mark_victim record\n");
	ksft_print_msg("status total=%ld anon=%ld file=%ld shmem=%ld kB; "
		       "trace total=%ld anon=%ld file=%ld shmem=%ld kB\n",
		       expected_total, expected_anon, expected_file, expected_shmem,
		       trace_stats.total_vm, trace_stats.anon_rss,
		       trace_stats.file_rss, trace_stats.shmem_rss);
	ksft_test_result(found && trace_stats.total_vm == expected_total,
			 "report OOM victim virtual memory in process-page bytes\n");
	expected_rss = expected_anon + expected_file + expected_shmem;
	traced_rss = trace_stats.anon_rss + trace_stats.file_rss +
		trace_stats.shmem_rss;
	ksft_test_result(found && within_tolerance(traced_rss, expected_rss),
			 "report OOM victim RSS in process-page bytes\n");
	ksft_test_result(trace_stats.anon_rss >= (long)(CHILD_MAP_SIZE / 1024),
			 "trace the victim's resident anonymous allocation\n");

	free(trace);
	write_text(TRACE_ENABLE, "0");
	ksft_finished();
}

/* The environment gate precedes the compat re-exec, as it always did. */
int main(int argc, char **argv)
{
	if (!getenv("PPPS_OOM_TRACE_TEST_FORCE"))
		ksft_exit_skip("forced OOM test requires an isolated VM\n");
	return ppps_compat_main(argc, argv, run_test);
}
