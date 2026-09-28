// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process executes a 4KB-aligned ELF whose segments must not be
 * mapped past its BSS, and the probe exits cleanly.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/wait.h>

#include "exec_p3s_helpers.h"
#include "p3s_harness.h"

TEST(exec_4k_aligned_elf)
{
	char probe_path[PATH_MAX];
	int status = 0;
	pid_t pid;

	ASSERT_TRUE(p3s_exec_sibling(probe_path, sizeof(probe_path), "elf_4k_align_probe"));

	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		execl(probe_path, "elf_4k_align_probe", NULL);
		_exit(127);
	}
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	if (WIFSIGNALED(status))
		TH_LOG("4K-aligned ELF died from signal %d",
		       WTERMSIG(status));
	else if (WIFEXITED(status) && WEXITSTATUS(status))
		TH_LOG("4K-aligned ELF exited with status %d",
		       WEXITSTATUS(status));
	EXPECT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status));
}

P3S_HARNESS_4KB_MAIN
