// SPDX-License-Identifier: GPL-2.0
/*
 * With ASLR enabled, page-local stack randomization of a 4KB exec
 * stays within a two-process-page RLIMIT_STACK across repeated execs.  The
 * probe runs with an empty environment: the 8K budget must hold the
 * arguments, the ELF tables and up to one process page of randomization.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "exec_p3s_helpers.h"
#include "p3s_harness.h"

#define ATTEMPTS 64

static bool collect_execs(const char *probe, unsigned int *failed)
{
	struct rlimit limit = {
		.rlim_cur = 2 * PROCESS_PAGE_SIZE,
		.rlim_max = 2 * PROCESS_PAGE_SIZE,
	};
	unsigned int i;

	*failed = 0;
	for (i = 0; i < ATTEMPTS; i++) {
		int status;
		pid_t pid = fork();

		if (pid < 0)
			return false;
		if (!pid) {
			char *const argv[] = { "elf_4k_align_probe", NULL };
			char *const envp[] = { NULL };

			if (setrlimit(RLIMIT_STACK, &limit))
				_exit(126);
			execve(probe, argv, envp);
			_exit(127);
		}
		if (waitpid(pid, &status, 0) != pid)
			return false;
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			(*failed)++;
	}
	return true;
}

TEST(randomized_exec_stays_within_stack_limit)
{
	char probe[PATH_MAX];
	int persona = personality(0xffffffffUL);
	unsigned int failed;
	bool collected;

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 0 ||
	    (persona >= 0 && (persona & ADDR_NO_RANDOMIZE)))
		SKIP(return, "address randomization is disabled");

	collected = p3s_exec_sibling(probe, sizeof(probe),
				     "elf_4k_align_probe") &&
		    collect_execs(probe, &failed);
	ASSERT_TRUE(collected) TH_LOG("could not collect exec samples");
	TH_LOG("failed exec samples: %u/%u", failed, ATTEMPTS);
	EXPECT_EQ(0, failed);
}

P3S_HARNESS_4KB_MAIN
