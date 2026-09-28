// SPDX-License-Identifier: GPL-2.0
/*
 * Repeated execs of a PIE with its own interpreter from a 4KB process
 * all fit in the address space and are never killed by an invalid ASLR base.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "p3s_harness.h"
#define P3S_ASLR_FIXTURE
#include "exec_p3s_helpers.h"

#define EXEC_COUNT	64
static char helper_path[PATH_MAX];
static char interp_path[PATH_MAX];

static bool find_helpers(void)
{
	return p3s_exec_sibling(helper_path, sizeof(helper_path),
			    "exec_aslr_p3s_helper") &&
	       p3s_exec_sibling(interp_path, sizeof(interp_path),
				"exec_aslr_p3s_interp") &&
	       !access(helper_path, X_OK) && !access(interp_path, R_OK | X_OK);
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
		execl(helper_path, "exec_aslr_p3s_helper", NULL);
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

TEST_F(aslr, p3s_exec_aslr)
{
	unsigned int succeeded = 0;
	unsigned int signaled = 0;
	unsigned int exited = 0;
	unsigned int i;

	ASSERT_TRUE(find_helpers()) TH_LOG("could not find helper executables");

	for (i = 0; i < EXEC_COUNT; i++)
		if (run_helper(&exited, &signaled))
			succeeded++;

	ksft_print_msg("attempts=%u succeeded=%u exited=%u signaled=%u\n",
		       EXEC_COUNT, succeeded, exited, signaled);
	EXPECT_TRUE(signaled == 0);
	EXPECT_TRUE(succeeded == EXEC_COUNT);
}

P3S_HARNESS_4KB_MAIN
