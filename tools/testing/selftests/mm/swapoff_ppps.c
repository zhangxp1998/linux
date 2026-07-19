// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/swap.h>
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
#define POLL_ATTEMPTS	200
#define SWAPOFF_TIMEOUT	3

struct child_status {
	long page_size;
	bool mapped;
	bool isolated;
	bool paged_out;
	unsigned long rss_bytes;
	unsigned long swap_bytes;
};

static bool swap_info(unsigned long *total_bytes, unsigned long *free_bytes)
{
	unsigned long total_kb = 0;
	unsigned long free_kb = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		return false;
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "SwapTotal: %lu kB", &total_kb) == 1)
			continue;
		if (sscanf(line, "SwapFree: %lu kB", &free_kb) == 1)
			continue;
	}
	free(line);
	fclose(meminfo);
	*total_bytes = total_kb * 1024;
	*free_bytes = free_kb * 1024;
	return true;
}

static bool vma_usage_bytes(const void *address, unsigned long *rss_bytes,
			    unsigned long *swap_bytes)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end, value_kb;
	char *line = NULL;
	size_t capacity = 0;
	bool found_rss = false;
	bool found_swap = false;
	bool in_target = false;
	FILE *smaps;

	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_target)
				break;
			in_target = target >= start && target < end;
			continue;
		}
		if (!in_target)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found_rss = true;
		} else if (sscanf(line, "Swap: %lu kB", &value_kb) == 1) {
			*swap_bytes = value_kb * 1024;
			found_swap = true;
		}
	}
	free(line);
	fclose(smaps);
	return found_rss && found_swap;
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

static bool page_out_mapping(void *mapping, unsigned long *rss_bytes,
			     unsigned long *swap_bytes)
{
	unsigned int attempt;

	if (madvise(mapping, MAPPING_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!vma_usage_bytes(mapping, rss_bytes, swap_bytes))
			return false;
		if (*swap_bytes >= MAPPING_SIZE / 2 &&
		    *rss_bytes <= MAPPING_SIZE / 2)
			return true;
		usleep(10000);
	}
	return false;
}

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

static int run_compat_child(int ready_fd, int command_fd)
{
	struct child_status status = {
		.page_size = sysconf(_SC_PAGESIZE),
	};
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	unsigned char command;
	bool preserved = true;
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
		status.mapped = true;
		status.isolated = vma_span(mapping, &span) &&
				  span == MAPPING_SIZE;
		status.paged_out = page_out_mapping(mapping,
						    &status.rss_bytes,
						    &status.swap_bytes);
	}

	if (!write_full(ready_fd, &status, sizeof(status)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		preserved = false;
	if (mapping == MAP_FAILED) {
		preserved = false;
	} else {
		for (i = 0; i < TEST_PAGES; i++) {
			if (mapping[i * USER_PAGE_SIZE] !=
			    (unsigned char)(0x40 + i)) {
				preserved = false;
				break;
			}
		}
		munmap(reservation, RESERVE_SIZE);
	}
	return preserved ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void alarm_handler(int signal)
{
	(void)signal;
}

static int run_parent(void)
{
	const char *swap_device = getenv("PPPS_SWAP_DEVICE");
	struct child_status child = {};
	struct sigaction action = {
		.sa_handler = alarm_handler,
	};
	unsigned long total_swap = 0;
	unsigned long free_swap = 0;
	int ready_pipe[2];
	int command_pipe[2];
	unsigned char command = 1;
	bool child_ready;
	bool disabled;
	bool child_waited;
	int child_wait_status = 0;
	int swapoff_errno;
	int swapoff_ret;
	char ready_fd[16];
	char command_fd[16];
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(9);
	if (!swap_device)
		ksft_exit_skip("PPPS_SWAP_DEVICE is not set\n");
	if (!swap_info(&total_swap, &free_swap))
		ksft_exit_fail_msg("could not read /proc/meminfo\n");
	ksft_test_result(total_swap && free_swap, "swap device is active\n");
	if (!total_swap || !free_swap)
		ksft_exit_fail_msg("swap device has no free space\n");

	if (pipe(ready_pipe) || pipe(command_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!pid) {
		int persona;

		close(ready_pipe[0]);
		close(command_pipe[1]);
		persona = personality(0xffffffffUL);
		if (persona < 0 ||
		    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
			_exit(127);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
		snprintf(command_fd, sizeof(command_fd), "%d", command_pipe[0]);
		execl("/proc/self/exe", "swapoff_ppps", "--child", ready_fd,
		      command_fd, NULL);
		_exit(127);
	}

	close(ready_pipe[1]);
	close(command_pipe[0]);
	child_ready = read_full(ready_pipe[0], &child, sizeof(child));
	close(ready_pipe[0]);
	ksft_test_result(child_ready, "start compat child process\n");
	if (!child_ready) {
		close(command_pipe[1]);
		waitpid(pid, NULL, 0);
		ksft_exit_fail_msg("compat child did not report status\n");
	}
	ksft_print_msg("swapoff caller page size: %ld\n", sysconf(_SC_PAGESIZE));
	ksft_test_result(child.page_size == USER_PAGE_SIZE,
			 "child process uses 4K pages\n");
	ksft_test_result(child.mapped && child.isolated,
			 "map an isolated anonymous VMA in the child\n");
	ksft_test_result(child.paged_out,
			 "child has swap PTEs (Swap: %lu, Rss: %lu bytes)\n",
			 child.swap_bytes, child.rss_bytes);

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	alarm(SWAPOFF_TIMEOUT);
	errno = 0;
	swapoff_ret = swapoff(swap_device);
	swapoff_errno = errno;
	alarm(0);
	ksft_test_result(!swapoff_ret,
			 "native caller restores compat PTEs with swapoff (%s)\n",
			 swapoff_ret ? strerror(swapoff_errno) : "success");

	write_full(command_pipe[1], &command, sizeof(command));
	close(command_pipe[1]);
	do {
		child_waited = waitpid(pid, &child_wait_status, 0) == pid;
	} while (!child_waited && errno == EINTR);
	ksft_test_result(child_waited && WIFEXITED(child_wait_status) &&
			 WEXITSTATUS(child_wait_status) == EXIT_SUCCESS,
			 "restored child pages preserve their contents\n");
	disabled = swap_info(&total_swap, &free_swap) && !total_swap;
	ksft_test_result(disabled, "swap device is disabled\n");
	ksft_test_result(!swapoff_ret && disabled,
			 "swapoff leaves no compat swap PTEs\n");
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
	execl("/proc/self/exe", "swapoff_ppps", "--parent", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_native_parent();
	if (argc == 2 && !strcmp(argv[1], "--parent"))
		return run_parent();
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_compat_child(atoi(argv[2]), atoi(argv[3]));
	return EXIT_FAILURE;
}
