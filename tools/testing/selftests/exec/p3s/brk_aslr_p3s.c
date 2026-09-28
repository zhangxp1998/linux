// SPDX-License-Identifier: GPL-2.0
/*
 * brk ASLR of a 4KB process places the initial brk at every 4KB
 * residue within a native 16K page across repeated execs.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/wait.h>

#include "exec_p3s_helpers.h"
#include "p3s_harness.h"

#define SAMPLE_COUNT 64

static char sample_path[PATH_MAX];

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
			execl(sample_path, "brk_aslr_p3s_sample", NULL);
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

		if (brk & (PROCESS_PAGE_SIZE - 1))
			return false;
		*residue_mask |= 1U << ((brk / PROCESS_PAGE_SIZE) & 3);
		if (brk < *minimum)
			*minimum = brk;
		if (brk > *maximum)
			*maximum = brk;
	}
	return true;
}

TEST(brk_uses_all_4k_residues)
{
	uintptr_t minimum, maximum;
	int persona = personality(0xffffffffUL);
	unsigned int residue_mask;
	bool collected;

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 1 ||
	    (persona >= 0 && (persona & ADDR_NO_RANDOMIZE)))
		SKIP(return, "brk ASLR is disabled");

	collected = p3s_exec_sibling(sample_path, sizeof(sample_path),
				     "brk_aslr_p3s_sample") &&
		collect_samples(&residue_mask, &minimum, &maximum);
	ASSERT_TRUE(collected) TH_LOG("failed to collect brk samples: %s", strerror(errno));

	TH_LOG("samples=%u min=%#lx max=%#lx 16K-residue-mask=%#x",
	       SAMPLE_COUNT, (unsigned long)minimum,
		       (unsigned long)maximum, residue_mask);
	EXPECT_EQ(0xf, residue_mask);
}

P3S_HARNESS_4KB_MAIN
