// SPDX-License-Identifier: GPL-2.0
/*
 * A packed anonymous folio remains discoverable by memory-failure rmap after
 * its first process-page slice is unmapped and the surviving VMA therefore
 * starts at a later slice offset.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "kselftest_p3s.h"
#include "p3s_harness.h"

#ifndef MADV_HWPOISON
#define MADV_HWPOISON 100
#endif

struct child_setup {
	int error;
	bool hidden_pfn;
	bool packed;
};

static void signal_exit(int signal)
{
	_exit(signal);
}

static void poison_later_slice(int status_fd)
{
	struct sigaction action = {
		.sa_handler = signal_exit,
	};
	uint64_t entry = 0, first = 0, sibling;
	struct child_setup setup = {};
	unsigned char *mapping;
	unsigned int i;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL) ||
	    sigaction(SIGALRM, &action, NULL)) {
		setup.error = errno;
		goto report;
	}

	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		setup.error = errno;
		goto report;
	}
	for (i = 0; i < NATIVE_PAGE_SIZE; i += PROCESS_PAGE_SIZE)
		mapping[i] = 0x40 + i / PROCESS_PAGE_SIZE;

	if (!p3s_pagemap_entry(mapping, &entry) || !(entry & PM_PRESENT) ||
	    !PAGEMAP_PFN(entry)) {
		setup.hidden_pfn = true;
		goto unmap_all;
	}
	first = p3s_native_pfn(PAGEMAP_PFN(entry));
	setup.packed = first != 0;
	for (i = 1; i < P3S_SLICES; i++)
		setup.packed &= p3s_pfn(mapping + i * PROCESS_PAGE_SIZE, &sibling) &&
				sibling == first;

	if (munmap(mapping, PROCESS_PAGE_SIZE)) {
		setup.error = errno;
		goto unmap_tail;
	}
	if (!write_full(status_fd, &setup, sizeof(setup)))
		_exit(3);
	close(status_fd);

	/* A fixed collect_procs_anon() finds this later-slice VMA and SIGBUSes. */
	if (madvise(mapping + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		    MADV_HWPOISON))
		_exit(errno == EINVAL || errno == EOPNOTSUPP ? 2 : 4);
	/* Give an asynchronously queued machine-check signal time to arrive. */
	alarm(2);
	for (;;)
		pause();

unmap_all:
	munmap(mapping, PROCESS_PAGE_SIZE);
unmap_tail:
	munmap(mapping + PROCESS_PAGE_SIZE,
	       NATIVE_PAGE_SIZE - PROCESS_PAGE_SIZE);
report:
	write_full(status_fd, &setup, sizeof(setup));
	_exit(setup.hidden_pfn ? 2 : 3);
}

TEST(p3s_memory_failure_anon)
{
	struct child_setup setup = {};
	int pipefd[2];
	int status;
	pid_t child;

	/* MADV_HWPOISON is persistent; never inject it during a normal test run. */
	if (!getenv("P3S_HWPOISON_TEST_FORCE"))
		SKIP(return, "set P3S_HWPOISON_TEST_FORCE=1 to inject HWPOISON");

	ASSERT_EQ(0, pipe2(pipefd, O_CLOEXEC));
	child = fork();
	if (!child) {
		close(pipefd[0]);
		poison_later_slice(pipefd[1]);
	}
	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		ASSERT_GE(child, 0);
	}
	close(pipefd[1]);
	ASSERT_TRUE(read_full(pipefd[0], &setup, sizeof(setup)));
	close(pipefd[0]);
	if (setup.hidden_pfn) {
		waitpid(child, &status, 0);
		SKIP(return, "pagemap PFNs require CAP_SYS_ADMIN");
	}
	ASSERT_EQ(0, setup.error);
	ASSERT_TRUE(setup.packed);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
		SKIP(return, "MADV_HWPOISON is unavailable");
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == SIGBUS);
}

P3S_HARNESS_4KB_MAIN
