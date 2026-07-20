// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
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
#define SAMPLE_COUNT 64

static bool collect_samples(unsigned int *residue_mask)
{
	unsigned int i;

	*residue_mask = 0;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		uintptr_t address;
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
			execl("/proc/self/exe", "mmap_aslr_ppps", "--sample",
			      NULL);
			_exit(127);
		}
		close(pipefd[1]);
		if (read(pipefd[0], &address, sizeof(address)) !=
		    sizeof(address)) {
			close(pipefd[0]);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return false;
		}
		close(pipefd[0]);
		if (waitpid(pid, &status, 0) != pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return false;
		*residue_mask |= 1U << ((address / USER_PAGE_SIZE) & 3);
	}
	return true;
}

static bool aslr_enabled(void)
{
	int randomize_va_space;
	FILE *file = fopen("/proc/sys/kernel/randomize_va_space", "re");

	if (!file)
		return false;
	if (fscanf(file, "%d", &randomize_va_space) != 1)
		randomize_va_space = 0;
	fclose(file);
	return randomize_va_space > 0;
}

static int run_test(void)
{
	unsigned int residue_mask;
	bool collected;

	ksft_print_header();
	if (!aslr_enabled())
		ksft_exit_skip("address randomization is disabled\n");
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	collected = collect_samples(&residue_mask);
	ksft_test_result(collected, "collect anonymous mmap bases across exec\n");
	if (!collected)
		ksft_exit_fail_msg("could not collect mmap samples\n");
	ksft_print_msg("16K mmap residue mask: %#x\n", residue_mask);
	ksft_test_result(residue_mask == 0xf,
			 "mmap ASLR uses every process-page residue within 16K\n");
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("could not enable 4K compatibility mode\n");
	execl("/proc/self/exe", "mmap_aslr_ppps", "--compat", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

static int sample(void)
{
	void *mapping;
	uintptr_t address;

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	address = (uintptr_t)mapping;
	if (write(3, &address, sizeof(address)) != sizeof(address))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return run_test();
	if (argc == 2 && !strcmp(argv[1], "--sample"))
		return sample();
	return EXIT_FAILURE;
}
