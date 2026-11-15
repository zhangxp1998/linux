// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process mapping a shmem (memfd) or secretmem file that is one
 * 4KB page long can access that page but takes SIGBUS on a fault at its 4KB
 * EOF, even though the backing native page is 16KB.  The file kind is
 * selected on the command line: "shmem" or "secretmem".
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#ifndef __NR_memfd_secret
#define __NR_memfd_secret 447
#endif

struct memfd_case {
	const char *name;
	int (*create)(void);
	const char *create_name;
};

static int create_shmem(void)
{
	return memfd_create("shmem-eof-p3s", 0);
}

static int create_secretmem(void)
{
	return syscall(__NR_memfd_secret, 0);
}

static const struct memfd_case memfd_cases[] = {
	{ "shmem", create_shmem, "memfd_create" },
	{ "secretmem", create_secretmem, "memfd_secret" },
};

static const struct memfd_case *selected_case;

static bool secretmem_mlock_refused(int err)
{
	return !strcmp(selected_case->name, "secretmem") &&
	       (err == EAGAIN || err == EPERM || err == ENOMEM);
}

static bool faults_with_sigbus(void *mapping)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return false;
	if (!pid) {
		unsigned char value;

		memcpy(&value, mapping, sizeof(value));
		_exit(value == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (waitpid(pid, &status, 0) != pid)
		return false;
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS;
}

FIXTURE(memfd_eof) {
	int fd;
};

FIXTURE_SETUP(memfd_eof)
{
	self->fd = selected_case->create();
	if (self->fd < 0) {
		if (errno == ENOSYS || p3s_fixture_unavailable(errno))
			SKIP(return, "%s unavailable: %s",
			     selected_case->create_name, strerror(errno));
		ASSERT_GE(self->fd, 0) TH_LOG("%s failed: %s",
					      selected_case->create_name,
					      strerror(errno));
	}
	ASSERT_EQ(0, ftruncate(self->fd, PROCESS_PAGE_SIZE));
}

FIXTURE_TEARDOWN(memfd_eof)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(memfd_eof, page_before_eof_is_accessible)
{
	unsigned char *valid;

	valid = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, self->fd, 0);
	if (valid == MAP_FAILED && secretmem_mlock_refused(errno))
		SKIP(return, "secretmem mlock limit exceeded");
	ASSERT_NE(valid, MAP_FAILED);
	valid[0] = 0x5a;
	EXPECT_EQ(0x5a, valid[0]);
	munmap(valid, PROCESS_PAGE_SIZE);
}

TEST_F(memfd_eof, fault_at_4kb_eof_raises_sigbus)
{
	unsigned char *valid;
	void *past_eof;

	valid = mmap(NULL, 2 * PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, self->fd, 0);
	if (valid == MAP_FAILED && secretmem_mlock_refused(errno))
		SKIP(return, "secretmem mlock limit exceeded");
	ASSERT_NE(valid, MAP_FAILED);
	valid[0] = 0x5a;
	EXPECT_TRUE(faults_with_sigbus(valid + PROCESS_PAGE_SIZE));
	munmap(valid, 2 * PROCESS_PAGE_SIZE);

	past_eof = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
			self->fd, PROCESS_PAGE_SIZE);
	if (past_eof == MAP_FAILED && secretmem_mlock_refused(errno))
		SKIP(return, "secretmem mlock limit exceeded");
	ASSERT_NE(past_eof, MAP_FAILED);
	EXPECT_TRUE(faults_with_sigbus(past_eof));
	munmap(past_eof, PROCESS_PAGE_SIZE);
}

static const struct memfd_case *find_case(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(memfd_cases); i++)
		if (!strcmp(memfd_cases[i].name, name))
			return &memfd_cases[i];
	return NULL;
}

static void setup_harness(int *argc, char ***argv)
{
	char *name = p3s_harness_take_positional(argc, argv);

	selected_case = name ? find_case(name) : NULL;
	if (!selected_case && !p3s_harness_listing(*argc, *argv))
		ksft_exit_fail_msg("usage: %s shmem|secretmem\n", (*argv)[0]);
}

P3S_HARNESS_4KB_MAIN
