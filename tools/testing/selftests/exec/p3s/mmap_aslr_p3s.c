// SPDX-License-Identifier: GPL-2.0
/*
 * mmap ASLR of a 4KB process randomizes the anonymous mapping base across
 * fresh execs in 4KB-page units, within the process address-space cap.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE sample_role
#include "p3s_harness.h"
#define P3S_ASLR_FIXTURE
#include "exec_p3s_helpers.h"

#define SAMPLE_COUNT 64
static bool collect_samples(unsigned int *distinct_bases,
			    uintptr_t *minimum, uintptr_t *maximum)
{
	uintptr_t first = 0;
	unsigned int i;

	*distinct_bases = 0;
	*minimum = UINTPTR_MAX;
	*maximum = 0;
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
			p3s_execl(true, NULL, "--sample", NULL);
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
		if (address & (PROCESS_PAGE_SIZE - 1))
			return false;
		if (!i)
			first = address;
		if (!i || address != first)
			(*distinct_bases)++;
		if (address < *minimum)
			*minimum = address;
		if (address > *maximum)
			*maximum = address;
	}
	return true;
}

TEST_F(aslr, mmap_base_varies_across_4k_execs)
{
	unsigned int distinct_bases;
	uintptr_t minimum, maximum;

	ASSERT_TRUE(collect_samples(&distinct_bases,
				    &minimum, &maximum));
	TH_LOG("%u distinct mmap bases, span %#lx",
	       distinct_bases, (unsigned long)(maximum - minimum));
	EXPECT_GT(distinct_bases, 1);
	/* 24 random bits of 4KB pages spread the base over at most 64 GiB. */
	EXPECT_LT((uint64_t)(maximum - minimum),
		  (1ULL << (P3S_MMAP_RND_BITS_CAP + 12)) + (4ULL << 30));
}

static int sample(void)
{
	void *mapping;
	uintptr_t address;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	address = (uintptr_t)mapping;
	if (write(3, &address, sizeof(address)) != sizeof(address))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static int sample_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--sample"))
		return sample();
	return -1;
}

P3S_HARNESS_4KB_MAIN
