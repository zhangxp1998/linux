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
#define MAPPING_SIZE (4 * USER_PAGE_SIZE)

static int run_test(void)
{
	static const uint8_t expected[] = { 0x11, 0x22, 0x33, 0x44 };
	uint8_t *map2;
	uint8_t *map1;
	uint8_t *mapping;
	int fd;
	int i;

	ksft_print_header();
	ksft_set_plan(11);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/uio_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the UIO test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one native 16K UIO region\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	for (i = 0; i < 4; i++)
		ksft_test_result(mapping[i * USER_PAGE_SIZE] == expected[i],
				 "4K slice %d maps the expected backing data\n", i);

	map1 = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, USER_PAGE_SIZE);
	ksft_test_result(map1 != MAP_FAILED,
			 "map UIO map1 with an offset of one process page\n");
	if (map1 == MAP_FAILED)
		ksft_exit_fail_msg("map1 mmap failed: %s\n", strerror(errno));
	ksft_test_result(map1[0] == 0x5a,
			 "one-process-page offset selects UIO map1 (value=0x%02x)\n",
			 map1[0]);
	map2 = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 2 * USER_PAGE_SIZE);
	ksft_test_result(map2 != MAP_FAILED,
			 "map UIO map2 with an offset of two process pages\n");
	if (map2 == MAP_FAILED)
		ksft_exit_fail_msg("map2 mmap failed: %s\n", strerror(errno));
	ksft_test_result(map2[0] == 0x6b,
			 "two-process-page offset selects UIO map2 (value=0x%02x)\n",
			 map2[0]);

	munmap(map2, USER_PAGE_SIZE);
	munmap(map1, USER_PAGE_SIZE);
	munmap(mapping, MAPPING_SIZE);
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
	execl("/proc/self/exe", "uio_mmap_ppps", "--run", NULL);
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
