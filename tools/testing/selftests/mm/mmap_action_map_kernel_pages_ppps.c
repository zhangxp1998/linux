// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"
#include "mmap_action_map_kernel_pages_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_PAGES	4
#define MAPPING_SIZE	(MAPPING_PAGES * USER_PAGE_SIZE)
#define FIRST_MARKER	0x81

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

static int run_test(void)
{
	struct sigaction action = {
		.sa_handler = fault_handler,
	};
	unsigned char *mapping;
	unsigned char value = 0;
	unsigned int page;
	int fd;

	ksft_print_header();
	ksft_set_plan(5 + MAPPING_PAGES);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/" MMAP_ACTION_MAP_KERNEL_PAGES_PPPS_DEVICE_NAME,
		  O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the map-kernel-pages test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one native 16K range through mmap_prepare\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) ||
	    sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	for (page = 0; page < MAPPING_PAGES; page++) {
		unsigned char expected = FIRST_MARKER + page;
		unsigned char value = 0;
		bool readable = read_byte(mapping + page * USER_PAGE_SIZE, &value);

		if (readable && value != expected)
			ksft_print_msg("slice %u value=%#x expected=%#x\n",
				       page, value, expected);
		ksft_test_result(readable && value == expected,
				 "slice %u is mapped with the expected marker\n",
				 page);
	}

	munmap(mapping, MAPPING_SIZE);
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one 4K slice through mmap_prepare\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("4K mmap failed: %s\n", strerror(errno));
	ksft_test_result(read_byte(mapping, &value) && value == FIRST_MARKER,
			 "the standalone 4K slice is mapped\n");
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
	execl("/proc/self/exe", "mmap_action_map_kernel_pages_ppps", "--run",
	      NULL);
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
