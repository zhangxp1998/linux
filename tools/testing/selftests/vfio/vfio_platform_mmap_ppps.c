// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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

static int verify_mapping(const unsigned char *mapping)
{
	int fd;
	int ret;

	fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return EXIT_FAILURE;
	ret = write(fd, mapping, 1) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
	close(fd);
	return ret;
}

static int run_test(void)
{
	unsigned char *mapping;
	int status;
	int fd;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/vfio_platform_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the VFIO platform fixture\n");
	if (fd < 0)
		ksft_exit_fail_msg("open fixture failed: %s\n", strerror(errno));

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "map one process page\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!pid)
		_exit(verify_mapping(mapping));
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			 "mapped VFIO platform page is accessible\n");

	munmap(mapping, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "vfio_platform_mmap_ppps", "--run", NULL);
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
