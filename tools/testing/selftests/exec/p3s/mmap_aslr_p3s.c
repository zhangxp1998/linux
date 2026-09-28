// SPDX-License-Identifier: GPL-2.0
/*
 * mmap ASLR of a 4K compat process randomizes the anonymous mapping base
 * across fresh execs at 4K process-page alignment.  The base is randomized
 * with the native mmap granularity, so its residue within a 16K page is not
 * required to vary.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "exec_p3s_helpers.h"
#include "p3s_harness.h"

#define SAMPLE_COUNT 64
#define MMAP_RND_BITS_PATH "/proc/sys/vm/mmap_rnd_bits"

FIXTURE(aslr) {
	int original_rnd_bits;
	bool restore;
};

FIXTURE_SETUP(aslr)
{
	self->original_rnd_bits = p3s_exec_sysctl(MMAP_RND_BITS_PATH);
	if (self->original_rnd_bits < 0)
		SKIP(return, "mmap_rnd_bits is unavailable");
	if (self->original_rnd_bits <= 24) {
		if (!p3s_exec_write_sysctl(MMAP_RND_BITS_PATH, 30))
			SKIP(return, "cannot raise mmap_rnd_bits to exercise the 4K cap");
		self->restore = true;
	}
	if (p3s_exec_sysctl(MMAP_RND_BITS_PATH) <= 24)
		SKIP(return, "mmap_rnd_bits cannot exercise the 4K cap");
}

FIXTURE_TEARDOWN_PARENT(aslr)
{
	if (self->restore)
		EXPECT_TRUE(p3s_exec_write_sysctl(MMAP_RND_BITS_PATH,
						   self->original_rnd_bits));
}

static bool collect_samples(unsigned int *residue_mask,
			    unsigned int *distinct_bases,
			    uintptr_t *minimum, uintptr_t *maximum)
{
	uintptr_t first = 0;
	unsigned int i;

	*residue_mask = 0;
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
		*residue_mask |= 1U << ((address / PROCESS_PAGE_SIZE) & 3);
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
	unsigned int residue_mask;
	unsigned int distinct_bases;
	uintptr_t minimum, maximum;

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 0)
		SKIP(return, "address randomization is disabled");

	ASSERT_TRUE(collect_samples(&residue_mask, &distinct_bases,
				    &minimum, &maximum));
	TH_LOG("16K mmap residue mask: %#x, %u distinct bases, span %#lx",
	       residue_mask, distinct_bases, (unsigned long)(maximum - minimum));
	EXPECT_GT(distinct_bases, 1);
	/* 24 random native-page bits yield at most 256 GiB of ASLR spread. */
	EXPECT_LT(maximum - minimum, (1UL << (24 + 14)) + (4UL << 30));
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

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], "--compat", NULL);
	if (argc == 2 && !strcmp(mode, "--compat")) {
		p3s_require_compat();
		return p3s_harness_run();
	}
	if (argc == 2 && !strcmp(mode, "--sample"))
		return sample();
	return EXIT_FAILURE;
}
