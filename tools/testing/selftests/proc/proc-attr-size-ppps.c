// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static int run_test(void)
{
	char *value;
	int fd;
	int ret;
	ssize_t written;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/proc/self/attr/fscreate", O_WRONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the SELinux fscreate attribute\n");
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));

	value = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(value != MAP_FAILED, "map two process pages\n");
	if (value == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(value, 0, USER_PAGE_SIZE);
	ret = mprotect(value + USER_PAGE_SIZE, USER_PAGE_SIZE, PROT_NONE);
	ksft_test_result(ret == 0, "protect the page after the attribute\n");
	if (ret)
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	errno = 0;
	written = write(fd, value, USER_PAGE_SIZE + 1);
	ksft_print_msg("fscreate write returned %zd, errno %d (%s)\n",
		       written, errno, strerror(errno));
	ksft_test_result(written == USER_PAGE_SIZE,
			 "truncate proc attributes to the process page\n");
	munmap(value, 2 * USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "proc_attr_size_ppps", "--run", NULL);
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
