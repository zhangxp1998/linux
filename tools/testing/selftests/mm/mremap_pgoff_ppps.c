// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define DEVICE_PATH "/dev/mremap_pgoff_ppps"
#define USER_PAGE_SIZE 4096UL

static int run_test(void)
{
	void *mapping;
	void *moved;
	void *guard;
	void *reserve;
	int fd;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open(DEVICE_PATH, O_RDONLY);
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", DEVICE_PATH,
				   strerror(errno));
	reserve = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reserve == MAP_FAILED)
		ksft_exit_fail_msg("reserve mmap failed: %s\n", strerror(errno));
	guard = reserve + USER_PAGE_SIZE;
	if (munmap(reserve, USER_PAGE_SIZE))
		ksft_exit_fail_msg("reserve munmap failed: %s\n", strerror(errno));
	mapping = mmap(reserve, USER_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, USER_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the test file at a 4K offset\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	errno = 0;
	moved = mremap(mapping, USER_PAGE_SIZE, 2 * USER_PAGE_SIZE,
		       MREMAP_MAYMOVE);
	ksft_test_result(moved != MAP_FAILED,
			 "preserve the process-page file offset during mremap\n");
	if (moved == MAP_FAILED)
		ksft_print_msg("mremap failed: %s\n", strerror(errno));
	else
		munmap(moved, 2 * USER_PAGE_SIZE);
	if (moved == MAP_FAILED)
		munmap(mapping, USER_PAGE_SIZE);
	munmap(guard, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "mremap_pgoff_ppps", "--run", NULL);
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
