// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static bool get_probe_path(char path[PATH_MAX])
{
	char *separator;
	ssize_t length;

	length = readlink("/proc/self/exe", path, PATH_MAX - 1);
	if (length < 0 || length == PATH_MAX - 1)
		return false;
	path[length] = '\0';
	separator = strrchr(path, '/');
	if (!separator)
		return false;
	if ((size_t)(separator - path) + sizeof("/elf_4k_align_probe") >
	    PATH_MAX)
		return false;
	strcpy(separator, "/elf_4k_align_probe");
	return true;
}

static void run_test(void)
{
	char probe_path[PATH_MAX];
	struct rlimit limit = {
		.rlim_cur = USER_PAGE_SIZE,
		.rlim_max = USER_PAGE_SIZE,
	};
	char *const argv[] = { (char *)"elf_4k_align_probe", NULL };
	char *const envp[] = { NULL };
	int status = 0;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (!get_probe_path(probe_path))
		ksft_exit_fail_msg("could not locate the ELF probe\n");

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed\n");
	if (!pid) {
		if (setrlimit(RLIMIT_STACK, &limit))
			_exit(126);
		execve(probe_path, argv, envp);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid failed\n");
	if (WIFSIGNALED(status))
		ksft_print_msg("tight-stack ELF died from signal %d\n",
			       WTERMSIG(status));
	else if (WIFEXITED(status) && WEXITSTATUS(status))
		ksft_print_msg("tight-stack ELF exited with status %d\n",
			       WEXITSTATUS(status));
	ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
			 "execute a 4K ELF with a one-page stack limit\n");
	ksft_finished();
}

static void exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed\n");
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE |
			ADDR_NO_RANDOMIZE) < 0)
		ksft_exit_fail_msg("personality set failed\n");
	execl("/proc/self/exe", "exec_stack_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed\n");
}

int main(int argc, char **argv)
{
	if (argc == 1)
		exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		run_test();
	return EXIT_FAILURE;
}
