// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
#define VMCLOCK_MAGIC 0x4b4c4356U

static int run_test(void)
{
	uint32_t *mapping;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/vmclock0", O_RDONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the vmclock device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open vmclock failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one 4K process page from vmclock\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("vmclock mmap failed: %s\n",
				   strerror(errno));

	ksft_test_result(*mapping == VMCLOCK_MAGIC,
			 "mapped vmclock page has the ABI magic\n");
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
	execl("/proc/self/exe", "vmclock_mmap_ppps", "--run", NULL);
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
