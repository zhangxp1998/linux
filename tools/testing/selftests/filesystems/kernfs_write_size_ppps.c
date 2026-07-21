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
	int saved_errno;
	ssize_t written;

	ksft_print_header();
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/sys/devices/system/cpu/cpu1/online",
		  O_WRONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open a writable sysfs attribute\n");
	if (fd < 0)
		ksft_exit_fail_msg("sysfs open failed: %s\n", strerror(errno));

	value = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(value != MAP_FAILED, "map two process pages\n");
	if (value == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(value, 'x', USER_PAGE_SIZE);
	ret = mprotect(value + USER_PAGE_SIZE, USER_PAGE_SIZE, PROT_NONE);
	ksft_test_result(ret == 0, "protect the following process page\n");
	if (ret)
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	errno = 0;
	written = write(fd, value, USER_PAGE_SIZE + 1);
	saved_errno = errno;
	ksft_print_msg("sysfs write returned %zd, errno %d (%s)\n",
		       written, saved_errno, strerror(saved_errno));
	ksft_test_result(written == -1 && saved_errno == EINVAL,
			 "limit default kernfs copying to the process page\n");
	close(fd);

	fd = open("/sys/fs/cgroup/cgroup.procs", O_WRONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open a cgroup kernfs control\n");
	if (fd < 0)
		ksft_exit_fail_msg("cgroup open failed: %s\n", strerror(errno));
	errno = 0;
	written = write(fd, value, USER_PAGE_SIZE + 1);
	saved_errno = errno;
	ksft_print_msg("cgroup write returned %zd, errno %d (%s)\n",
		       written, saved_errno, strerror(saved_errno));
	ksft_test_result(written == -1 && saved_errno == E2BIG,
			 "limit atomic kernfs writes to the process page\n");
	close(fd);
	munmap(value, 2 * USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "kernfs_write_size_ppps", "--run", NULL);
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
