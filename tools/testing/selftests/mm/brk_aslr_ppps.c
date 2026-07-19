// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define SAMPLE_COUNT 64

static char sample_path[PATH_MAX];

static bool find_sample(void)
{
	char *filename;
	size_t dir_length;
	ssize_t length;

	length = readlink("/proc/self/exe", sample_path,
			  sizeof(sample_path) - 1);
	if (length < 0 || (size_t)length >= sizeof(sample_path) - 1)
		return false;
	sample_path[length] = '\0';
	filename = strrchr(sample_path, '/');
	if (!filename)
		return false;
	dir_length = filename + 1 - sample_path;
	if (dir_length + sizeof("brk_aslr_ppps_sample") > sizeof(sample_path))
		return false;
	memcpy(sample_path + dir_length, "brk_aslr_ppps_sample",
	       sizeof("brk_aslr_ppps_sample"));
	return true;
}

static bool read_full(int fd, void *buffer, size_t size)
{
	char *p = buffer;

	while (size) {
		ssize_t ret = read(fd, p, size);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return false;
		p += ret;
		size -= ret;
	}
	return true;
}

static bool collect_samples(unsigned int *residue_mask,
			    uintptr_t *minimum, uintptr_t *maximum)
{
	unsigned int i;

	*residue_mask = 0;
	*minimum = UINTPTR_MAX;
	*maximum = 0;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		uintptr_t brk;
		int pipefd[2];
		int status;
		pid_t pid;

		if (pipe(pipefd))
			return false;
		pid = fork();
		if (pid < 0) {
			close(pipefd[0]);
			close(pipefd[1]);
			return false;
		}
		if (!pid) {
			close(pipefd[0]);
			if (pipefd[1] != 3) {
				if (dup2(pipefd[1], 3) < 0)
					_exit(126);
				close(pipefd[1]);
			}
			execl(sample_path, "brk_aslr_ppps_sample", NULL);
			_exit(127);
		}
		close(pipefd[1]);
		if (!read_full(pipefd[0], &brk, sizeof(brk))) {
			close(pipefd[0]);
			waitpid(pid, &status, 0);
			return false;
		}
		close(pipefd[0]);
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
		    WEXITSTATUS(status))
			return false;

		*residue_mask |= 1U << ((brk / USER_PAGE_SIZE) & 3);
		if (brk < *minimum)
			*minimum = brk;
		if (brk > *maximum)
			*maximum = brk;
	}
	return true;
}

static bool brk_aslr_enabled(void)
{
	int randomize_va_space;
	FILE *file;

	file = fopen("/proc/sys/kernel/randomize_va_space", "r");
	if (!file)
		return false;
	if (fscanf(file, "%d", &randomize_va_space) != 1)
		randomize_va_space = 0;
	fclose(file);
	return randomize_va_space > 1;
}

static int run_test(void)
{
	uintptr_t minimum, maximum;
	unsigned int residue_mask;
	bool collected;

	ksft_print_header();
	if (!brk_aslr_enabled())
		ksft_exit_skip("brk ASLR is disabled\n");
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	collected = find_sample() &&
		collect_samples(&residue_mask, &minimum, &maximum);
	ksft_test_result(collected, "collect randomized brk samples across exec\n");
	if (!collected)
		ksft_exit_fail_msg("failed to collect brk samples: %s\n",
				   strerror(errno));

	ksft_print_msg("samples=%u min=%#lx max=%#lx 16K-residue-mask=%#x\n",
		       SAMPLE_COUNT, (unsigned long)minimum,
		       (unsigned long)maximum, residue_mask);
	ksft_test_result(residue_mask == 0xf,
			 "brk ASLR uses all process-page residues within 16K\n");
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
	execl("/proc/self/exe", "brk_aslr_ppps", "--run", NULL);
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
