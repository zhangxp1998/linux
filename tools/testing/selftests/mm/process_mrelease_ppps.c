// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * USER_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * USER_PAGE_SIZE)

struct child_status {
	long page_size;
	unsigned long address;
	bool mapped;
	bool isolated;
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

static bool vma_span(const void *address, unsigned long *span)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return false;
	while (getline(&line, &capacity, maps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;
		if (target >= start && target < end) {
			*span = end - start;
			found = true;
			break;
		}
	}
	free(line);
	fclose(maps);
	return found;
}

static bool read_smaps_rss(pid_t pid, unsigned long address,
			   unsigned long *rss_bytes)
{
	unsigned long start, end, value_kb;
	char path[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	FILE *smaps;

	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	smaps = fopen(path, "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_target)
				break;
			in_target = address >= start && address < end;
			continue;
		}
		if (in_target && sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found = true;
			break;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

static int run_compat_child(int ready_fd)
{
	struct child_status status = {
		.page_size = sysconf(_SC_PAGESIZE),
	};
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	unsigned int i;

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mapping = reservation == MAP_FAILED ? MAP_FAILED :
		mmap(reservation + USER_PAGE_SIZE, MAPPING_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping != MAP_FAILED) {
		madvise(mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
		for (i = 0; i < TEST_PAGES; i++)
			mapping[i * USER_PAGE_SIZE] = 0x40 + i;
		status.address = (unsigned long)mapping;
		status.mapped = true;
		status.isolated = vma_span(mapping, &span) &&
				  span == MAPPING_SIZE;
	}

	if (!write_full(ready_fd, &status, sizeof(status)))
		return EXIT_FAILURE;
	for (;;)
		pause();
}

static int run_parent(void)
{
	struct child_status child = {};
	struct sched_param realtime = { .sched_priority = 1 };
	struct sched_param normal = {};
	unsigned long rss_after = ~0UL;
	unsigned long rss_before = 0;
	int child_wait_status = 0;
	int ready_pipe[2];
	bool child_ready;
	bool before_ok;
	bool after_ok;
	bool release_ok = false;
	int release_errno = 0;
	char ready_fd[16];
	int pidfd = -1;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE ||
			 sysconf(_SC_PAGESIZE) == 4 * USER_PAGE_SIZE,
			 "parent uses a supported page size\n");
	if (pipe(ready_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!pid) {
		int persona;

		close(ready_pipe[0]);
		persona = personality(0xffffffffUL);
		if (persona < 0 ||
		    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
			_exit(127);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
		execl("/proc/self/exe", "process_mrelease_ppps", "--child",
		      ready_fd, NULL);
		_exit(127);
	}

	close(ready_pipe[1]);
	child_ready = read_full(ready_pipe[0], &child, sizeof(child));
	close(ready_pipe[0]);
	ksft_test_result(child_ready, "start compat child process\n");
	if (!child_ready) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		ksft_exit_fail_msg("compat child did not report status\n");
	}
	ksft_print_msg("process_mrelease caller page size: %ld\n",
		       sysconf(_SC_PAGESIZE));
	ksft_test_result(child.page_size == USER_PAGE_SIZE,
			 "child process uses 4K pages\n");
	ksft_test_result(child.mapped && child.isolated,
			 "map an isolated anonymous VMA in the child\n");
	before_ok = read_smaps_rss(pid, child.address, &rss_before);
	ksft_test_result(before_ok && rss_before == MAPPING_SIZE,
			 "child VMA is resident before release (%lu bytes)\n",
			 rss_before);

	pidfd = syscall(SYS_pidfd_open, pid, 0);
	ksft_test_result(pidfd >= 0, "open a pidfd for the child\n");
	if (pidfd < 0)
		goto out_kill;
	if (sched_setscheduler(0, SCHED_FIFO, &realtime))
		ksft_exit_fail_msg("SCHED_FIFO failed: %s\n", strerror(errno));
	if (kill(pid, SIGKILL)) {
		release_errno = errno;
	} else {
		release_ok = syscall(SYS_process_mrelease, pidfd, 0) == 0;
		release_errno = errno;
	}
	after_ok = read_smaps_rss(pid, child.address, &rss_after);
	if (sched_setscheduler(0, SCHED_OTHER, &normal))
		ksft_exit_fail_msg("SCHED_OTHER failed: %s\n", strerror(errno));
	ksft_test_result(release_ok, "process_mrelease succeeds (%s)\n",
			 release_ok ? "ok" : strerror(release_errno));
	ksft_test_result(after_ok && rss_after == 0,
			 "process_mrelease zaps the child VMA (%lu bytes)\n",
			 rss_after);
	goto out_wait;

out_kill:
	kill(pid, SIGKILL);
out_wait:
	waitpid(pid, &child_wait_status, 0);
	if (pidfd >= 0)
		close(pidfd);
	ksft_finished();
}

static int exec_native_parent(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality clear failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "process_mrelease_ppps", "--parent", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_native_parent();
	if (argc == 2 && !strcmp(argv[1], "--parent"))
		return run_parent();
	if (argc == 3 && !strcmp(argv[1], "--child"))
		return run_compat_child(atoi(argv[2]));
	return EXIT_FAILURE;
}
