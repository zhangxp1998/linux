// SPDX-License-Identifier: GPL-2.0
/*
 * Native and 4K compat exec keep the stack VMA end 16K-aligned while
 * retaining address randomization across repeated execs.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <signal.h>
#include <sys/wait.h>

#include "exec_p3s_helpers.h"
#include "p3s_harness.h"

#define SAMPLE_COUNT 64

static bool read_stack_end(pid_t pid, uintptr_t *stack_end)
{
	char path[64];
	char *line = NULL;
	size_t line_size = 0;
	bool found = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "/proc/%d/maps", pid) >=
	    (int)sizeof(path))
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end;

		if (strstr(line, "[stack]") &&
		    sscanf(line, "%lx-%lx", &start, &end) == 2) {
			*stack_end = end;
			found = true;
			break;
		}
	}
	free(line);
	fclose(file);
	return found;
}

static bool collect_samples(bool compat, unsigned int *residue_mask,
			    bool *varied)
{
	uintptr_t first = 0;
	unsigned int i;

	*residue_mask = 0;
	*varied = false;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		uintptr_t stack_end;
		unsigned char ready;
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
			p3s_execl(compat, NULL,
				  compat ? "--sample-compat" : "--sample-native",
				  NULL);
			_exit(127);
		}
		close(pipefd[1]);
		if (read(pipefd[0], &ready, 1) != 1 ||
		    !read_stack_end(pid, &stack_end)) {
			close(pipefd[0]);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return false;
		}
		close(pipefd[0]);
		*residue_mask |= 1U << ((stack_end / PROCESS_PAGE_SIZE) & 3);
		if (!i)
			first = stack_end;
		else if (stack_end != first)
			*varied = true;
		kill(pid, SIGKILL);
		if (waitpid(pid, &status, 0) != pid)
			return false;
	}
	return true;
}

TEST(native_stack_end_is_aligned_and_randomized)
{
	unsigned int residue_mask;
	bool varied;

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 0)
		SKIP(return, "address randomization is disabled");
	ASSERT_TRUE(collect_samples(false, &residue_mask, &varied));
	TH_LOG("native 16K stack-end residue mask: %#x", residue_mask);
	EXPECT_EQ(1, residue_mask);
	EXPECT_TRUE(varied);
}

TEST(compat_stack_end_is_aligned_and_randomized)
{
	unsigned int residue_mask;
	bool varied;

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 0)
		SKIP(return, "address randomization is disabled");
	ASSERT_TRUE(collect_samples(true, &residue_mask, &varied));
	TH_LOG("4K compat stack-end residue mask: %#x", residue_mask);
	EXPECT_EQ(1, residue_mask);
	EXPECT_TRUE(varied);
}

static int sample(bool compat)
{
	unsigned char ready = 1;

	if (compat ? !p3s_is_compat_process() :
	    getpagesize() != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	if (write(3, &ready, 1) != 1)
		return EXIT_FAILURE;
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], P3S_RUN_FLAG, NULL);
	if (argc == 2 && !strcmp(mode, P3S_RUN_FLAG)) {
		p3s_require_compat();
		return p3s_harness_run();
	}
	if (argc == 2 && !strcmp(mode, "--sample-compat"))
		return sample(true);
	if (argc == 2 && !strcmp(mode, "--sample-native"))
		return sample(false);
	return EXIT_FAILURE;
}
