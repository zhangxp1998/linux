// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
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

#define USER_PAGE_SIZE 4096UL
#define CHILD_MAP_SIZE (32UL * 1024 * 1024)
#define TRACE_ENABLE "/sys/kernel/tracing/events/oom/mark_victim/enable"
#define TRACE_FILE "/sys/kernel/tracing/trace"

struct victim_stats {
	long total_vm;
	long anon_rss;
	long file_rss;
	long shmem_rss;
};

struct victim_ready {
	uintptr_t mapping;
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

static unsigned long mapping_kernel_page_size(pid_t pid, uintptr_t address)
{
	char path[64];
	char *line = NULL;
	size_t line_size = 0;
	unsigned long page_size = 0;
	bool in_mapping = false;
	FILE *file;

	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	file = fopen(path, "re");
	if (!file)
		return 0;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (in_mapping &&
		    sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1) {
			page_size = size_kb * 1024;
			break;
		}
	}
	free(line);
	fclose(file);
	return page_size;
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
	struct victim_ready ready;
	unsigned char *mapping;
	size_t offset;

	if (write_text("/proc/self/oom_score_adj", "1000"))
		_exit(2);
	mapping = mmap(NULL, CHILD_MAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		_exit(3);
	for (offset = 0; offset < CHILD_MAP_SIZE; offset += USER_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset;
	ready.mapping = (uintptr_t)mapping;
	if (write(ready_fd, &ready, sizeof(ready)) != sizeof(ready))
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
	struct victim_ready ready = { 0 };
	struct victim_stats trace_stats = { 0 };
	long expected_anon_physical;
	long expected_anon, expected_file, expected_shmem, expected_total;
	unsigned long kernel_page_size;
	char *trace = NULL;
	bool anon_matches;
	bool child_ready;
	bool trace_ready;
	bool found = false;
	bool shared_matches;
	int pipefd[2];
	int status = 0;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

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
	child_ready = read(pipefd[0], &ready, sizeof(ready)) == sizeof(ready);
	close(pipefd[0]);
	kernel_page_size = mapping_kernel_page_size(child, ready.mapping);
	expected_total = read_status_kb(child, "VmSize");
	expected_anon = read_status_kb(child, "RssAnon");
	expected_file = read_status_kb(child, "RssFile");
	expected_shmem = read_status_kb(child, "RssShmem");
	child_ready = child_ready && kernel_page_size >= USER_PAGE_SIZE &&
		!(kernel_page_size % USER_PAGE_SIZE) && expected_total > 0 &&
		expected_anon > 0 && expected_file >= 0 && expected_shmem >= 0;
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
	expected_anon_physical = expected_anon * kernel_page_size /
		USER_PAGE_SIZE;
	ksft_print_msg("native_page=%lu; status total=%ld anon=%ld file=%ld shmem=%ld kB; "
		       "trace total=%ld anon=%ld file=%ld shmem=%ld kB\n",
		       kernel_page_size, expected_total, expected_anon,
		       expected_file, expected_shmem,
		       trace_stats.total_vm, trace_stats.anon_rss,
		       trace_stats.file_rss, trace_stats.shmem_rss);
	ksft_test_result(found && trace_stats.total_vm == expected_total,
			 "report OOM victim virtual memory in process-page bytes\n");
	shared_matches = within_tolerance(trace_stats.file_rss, expected_file) &&
		within_tolerance(trace_stats.shmem_rss, expected_shmem);
	ksft_test_result(found && shared_matches,
			 "report file and shmem RSS in process-page bytes\n");
	anon_matches = within_tolerance(trace_stats.anon_rss,
					expected_anon_physical);
	ksft_test_result(found && anon_matches,
			 "report anonymous RSS in native physical bytes\n");

	free(trace);
	write_text(TRACE_ENABLE, "0");
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n", strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n", strerror(errno));
	execl("/proc/self/exe", "oom_trace_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	(void)argv;
	if (!getenv("PPPS_OOM_TRACE_TEST_FORCE"))
		ksft_exit_skip("forced OOM test requires an isolated VM\n");
	if (sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE)
		return run_test();
	if (argc == 1)
		return exec_compat();
	ksft_exit_skip("4K compatibility process is unavailable\n");
}
