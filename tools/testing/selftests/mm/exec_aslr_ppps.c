// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define EXEC_COUNT	64

static char helper_path[PATH_MAX];
static char interp_path[PATH_MAX];

static bool find_sibling(char *path, size_t path_size, const char *name)
{
	char *filename;
	size_t dir_length;
	ssize_t length;

	length = readlink("/proc/self/exe", path, path_size - 1);
	if (length < 0 || (size_t)length >= path_size - 1)
		return false;
	path[length] = '\0';
	filename = strrchr(path, '/');
	if (!filename)
		return false;
	dir_length = filename + 1 - path;
	if (dir_length + strlen(name) + 1 > path_size)
		return false;
	memcpy(path + dir_length, name, strlen(name) + 1);
	return true;
}

static bool find_helpers(void)
{
	return find_sibling(helper_path, sizeof(helper_path),
			    "exec_aslr_ppps_helper") &&
	       find_sibling(interp_path, sizeof(interp_path),
			    "exec_aslr_ppps_interp") &&
	       !access(helper_path, X_OK) && !access(interp_path, R_OK);
}

static bool run_helper(unsigned int *exited, unsigned int *signaled)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		struct rlimit core_limit = {};
		int interp_fd = open(interp_path, O_RDONLY);

		if (setrlimit(RLIMIT_CORE, &core_limit))
			_exit(124);
		if (interp_fd < 0)
			_exit(125);
		if (interp_fd != 3) {
			if (dup2(interp_fd, 3) < 0)
				_exit(126);
			close(interp_fd);
		}
		execl(helper_path, "exec_aslr_ppps_helper", NULL);
		_exit(127);
	}

	if (waitpid(child, &status, 0) != child)
		return false;
	if (WIFEXITED(status)) {
		(*exited)++;
		return WEXITSTATUS(status) == 0;
	}
	if (WIFSIGNALED(status))
		(*signaled)++;
	return false;
}

static int run_test(void)
{
	unsigned int succeeded = 0;
	unsigned int signaled = 0;
	unsigned int exited = 0;
	unsigned int i;
	bool helpers_found;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	helpers_found = find_helpers();
	ksft_test_result(helpers_found,
			 "find the PIE executable and its interpreter\n");
	if (!helpers_found)
		ksft_exit_fail_msg("could not find helper executables\n");

	for (i = 0; i < EXEC_COUNT; i++)
		if (run_helper(&exited, &signaled))
			succeeded++;

	ksft_print_msg("attempts=%u succeeded=%u exited=%u signaled=%u\n",
		       EXEC_COUNT, succeeded, exited, signaled);
	ksft_test_result(signaled == 0,
			 "PIE execs are not killed by an invalid ASLR base\n");
	ksft_test_result(succeeded == EXEC_COUNT,
			 "all PIE execs fit in the process address space\n");
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
	execl("/proc/self/exe", "exec_aslr_ppps", "--run", NULL);
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
