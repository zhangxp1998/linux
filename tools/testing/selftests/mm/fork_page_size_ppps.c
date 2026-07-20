// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
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

#define USER_PAGE_SIZE	4096UL
#define WAIT_STEP_US	1000
#define WAIT_TIMEOUT_US	2000000

static unsigned long read_mmu_page_size(uintptr_t address)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	unsigned long result = 0;
	FILE *file;

	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return 0;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (in_mapping &&
		    sscanf(line, "MMUPageSize: %lu kB", &size_kb) == 1) {
			result = size_kb * 1024;
			break;
		}
	}
	free(line);
	fclose(file);
	return result;
}

static bool wait_for_child(pid_t child)
{
	int elapsed, status;

	for (elapsed = 0; elapsed < WAIT_TIMEOUT_US;
	     elapsed += WAIT_STEP_US) {
		pid_t waited = waitpid(child, &status, WNOHANG);

		if (waited == child)
			return WIFEXITED(status) &&
			       WEXITSTATUS(status) == EXIT_SUCCESS;
		if (waited < 0)
			return false;
		usleep(WAIT_STEP_US);
	}
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
	return false;
}

static int run_fork_case(bool compat)
{
	unsigned long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	int persona;
	pid_t child;
	size_t i;

	if (compat && page_size != USER_PAGE_SIZE)
		return EXIT_FAILURE;
	mapping = mmap(NULL, 4 * page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	for (i = 0; i < 4 * page_size; i++)
		mapping[i] = (unsigned char)(i * 37 + 11);

	persona = personality(0xffffffffUL);
	if (persona < 0)
		return EXIT_FAILURE;
	if (compat)
		persona &= ~ADDR_4KB_COMPAT_PAGE_SIZE;
	else
		persona |= ADDR_4KB_COMPAT_PAGE_SIZE;
	if (personality((unsigned int)persona) < 0)
		return EXIT_FAILURE;

	child = fork();
	if (child < 0)
		return EXIT_FAILURE;
	if (!child) {
		unsigned long mmu_page_size;

		for (i = 0; i < 4 * page_size; i++) {
			if (mapping[i] != (unsigned char)(i * 37 + 11))
				_exit(EXIT_FAILURE);
		}
		mmu_page_size = read_mmu_page_size((uintptr_t)mapping);
		dprintf(STDOUT_FILENO,
			"# %s fork: exec page size %lu, MMU page size %lu\n",
			compat ? "compat" : "native", page_size,
			mmu_page_size);
		_exit(mmu_page_size == page_size ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	return wait_for_child(child) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int reexec_case(bool compat)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		return EXIT_FAILURE;
	if (compat)
		persona |= ADDR_4KB_COMPAT_PAGE_SIZE;
	else
		persona &= ~ADDR_4KB_COMPAT_PAGE_SIZE;
	if (personality((unsigned int)persona) < 0)
		return EXIT_FAILURE;
	execl("/proc/self/exe", "fork_page_size_ppps",
	      compat ? "--compat" : "--native", NULL);
	return EXIT_FAILURE;
}

static bool run_case(bool compat)
{
	pid_t child = fork();

	if (child < 0)
		return false;
	if (!child)
		_exit(reexec_case(compat));
	return wait_for_child(child);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native"))
		return run_fork_case(false);
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return run_fork_case(true);
	if (argc != 1)
		return EXIT_FAILURE;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(run_case(false),
			 "fork preserves a native process page size\n");
	ksft_test_result(run_case(true),
			 "fork preserves a 4K compat process page size\n");
	ksft_finished();
}
