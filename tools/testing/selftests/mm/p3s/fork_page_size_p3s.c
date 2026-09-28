// SPDX-License-Identifier: GPL-2.0
/*
 * fork() preserves the parent's page size, native or 4KB, even when
 * the ADDR_PAGE_SIZE_4KB personality bit has been flipped before the
 * fork: the child sees the parent's data and smaps MMUPageSize.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define WAIT_STEP_US	1000
#define WAIT_TIMEOUT_US	2000000

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

static int run_fork_case(bool four_kb)
{
	unsigned long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	int persona, previous;
	pid_t child;
	size_t i;

	if (page_size != (four_kb ? PROCESS_PAGE_SIZE : NATIVE_PAGE_SIZE))
		return EXIT_FAILURE;
	mapping = mmap(NULL, 4 * page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	for (i = 0; i < 4 * page_size; i++)
		mapping[i] = (unsigned char)(i * 37 + 11);

	persona = personality(0xffffffffUL);
	if (persona < 0 || !!(persona & ADDR_PAGE_SIZE_4KB) != four_kb)
		return EXIT_FAILURE;
	previous = persona;
	if (four_kb)
		persona &= ~ADDR_PAGE_SIZE_4KB;
	else
		persona |= ADDR_PAGE_SIZE_4KB;
	/* Exercise libc's signed int return and an unrelated personality flag. */
	persona |= UNAME26;
	if (personality((unsigned int)persona) != previous ||
	    personality(0xffffffffUL) != persona)
		return EXIT_FAILURE;

	child = fork();
	if (child < 0)
		return EXIT_FAILURE;
	if (!child) {
		unsigned long mmu_page_size;

		if (personality(0xffffffffUL) != persona)
			_exit(EXIT_FAILURE);
		for (i = 0; i < 4 * page_size; i++) {
			if (mapping[i] != (unsigned char)(i * 37 + 11))
				_exit(EXIT_FAILURE);
		}
		if (!p3s_smaps_bytes(mapping, 1, "MMUPageSize",
				     &mmu_page_size))
			mmu_page_size = 0;
		dprintf(STDOUT_FILENO,
			"# %s fork: exec page size %lu, MMU page size %lu\n",
			four_kb ? "four_kb" : "native", page_size,
			mmu_page_size);
		_exit(mmu_page_size == page_size ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	return wait_for_child(child) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void check_fork(struct __test_metadata *_metadata, bool four_kb)
{
	int ret = p3s_run_child(four_kb,
				four_kb ? "--4kb" : "--check-native", NULL);

	if (ret == KSFT_SKIP)
		SKIP(return, "requested geometry unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(fork_preserves_native_page_size)
{
	check_fork(_metadata, false);
}

TEST(fork_preserves_4kb_page_size)
{
	check_fork(_metadata, true);
}

static int child_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--check-native"))
		return run_fork_case(false);
	if (argc == 2 && !strcmp(argv[1], "--4kb"))
		return run_fork_case(true);
	return -1;
}

P3S_HARNESS_4KB_MAIN
