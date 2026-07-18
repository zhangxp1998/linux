// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static bool faults_with_sigbus(void *mapping)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!pid) {
		unsigned char value;

		memcpy(&value, mapping, sizeof(value));
		_exit(value == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS;
}

static int run_test(void)
{
	unsigned char *valid;
	void *past_eof;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = memfd_create("shmem-eof-ppps", 0);
	if (fd < 0)
		ksft_exit_fail_msg("memfd_create failed: %s\n", strerror(errno));
	if (ftruncate(fd, USER_PAGE_SIZE))
		ksft_exit_fail_msg("ftruncate failed: %s\n", strerror(errno));

	valid = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	ksft_test_result(valid != MAP_FAILED,
			 "map the first shmem page\n");
	if (valid == MAP_FAILED)
		ksft_exit_fail_msg("valid mmap failed: %s\n", strerror(errno));
	valid[0] = 0x5a;
	ksft_test_result(valid[0] == 0x5a,
			 "fault and access shmem data before EOF\n");

	past_eof = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			USER_PAGE_SIZE);
	if (past_eof == MAP_FAILED)
		ksft_exit_fail_msg("past-EOF mmap failed: %s\n", strerror(errno));
	ksft_test_result(faults_with_sigbus(past_eof),
			 "SIGBUS a shmem fault at the 4K EOF\n");

	munmap(past_eof, USER_PAGE_SIZE);
	munmap(valid, USER_PAGE_SIZE);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "shmem_eof_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
