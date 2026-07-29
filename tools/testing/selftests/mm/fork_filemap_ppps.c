// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE		4096UL
#define NATIVE_PAGE_SIZE	16384UL
#define TARGET_OFFSET		USER_PAGE_SIZE
#define POISON_OFFSET		(3 * USER_PAGE_SIZE)
#define POISON_MAPPINGS		256
#define POISON_STRIDE		(2 * NATIVE_PAGE_SIZE)

static int run_test(void)
{
	unsigned char expected[USER_PAGE_SIZE];
	unsigned char *poison_region;
	unsigned char *poison_base;
	size_t poison_region_size;
	unsigned char *mapping;
	struct stat statbuf;
	pid_t child;
	int fd;
	int status;
	int i;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the test executable\n");
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));
	if (fstat(fd, &statbuf) ||
	    statbuf.st_size < (off_t)(4 * USER_PAGE_SIZE))
		ksft_exit_fail_msg("test executable is too small\n");
	if (pread(fd, expected, sizeof(expected), TARGET_OFFSET) !=
	    sizeof(expected))
		ksft_exit_fail_msg("pread failed: %s\n", strerror(errno));

	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd,
		       TARGET_OFFSET);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a non-native-aligned file page\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("target mmap failed: %s\n", strerror(errno));
	ksft_test_result(!memcmp(mapping, expected, sizeof(expected)),
			 "parent reads the expected file page\n");

	/*
	 * Fill the VMA slab freelist with objects carrying a different PPPS
	 * slice offset.  A fork must initialize every field of the duplicated
	 * VMA rather than depending on stale slab contents.
	 */
	poison_region_size = (POISON_MAPPINGS + 1) * POISON_STRIDE;
	poison_region = mmap(NULL, poison_region_size, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (poison_region == MAP_FAILED)
		ksft_exit_fail_msg("poison reservation failed: %s\n",
				   strerror(errno));
	poison_base = (unsigned char *)(((uintptr_t)poison_region +
					NATIVE_PAGE_SIZE - 1) &
				       ~(NATIVE_PAGE_SIZE - 1));
	for (i = 0; i < POISON_MAPPINGS; i++) {
		unsigned char *address = poison_base + i * POISON_STRIDE +
					 POISON_OFFSET;
		void *poison;

		poison = mmap(address, USER_PAGE_SIZE, PROT_READ,
			      MAP_PRIVATE | MAP_FIXED, fd, POISON_OFFSET);
		if (poison != address)
			ksft_exit_fail_msg("poison mmap failed: %s\n",
					   strerror(errno));
	}
	for (i = 0; i < POISON_MAPPINGS; i++) {
		unsigned char *address = poison_base + i * POISON_STRIDE;

		if (munmap(address, NATIVE_PAGE_SIZE))
			ksft_exit_fail_msg("poison munmap failed: %s\n",
					   strerror(errno));
	}

	child = fork();
	ksft_test_result(child >= 0, "fork the process\n");
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child)
		_exit(memcmp(mapping, expected, sizeof(expected)) ? 1 : 0);

	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			 "child faults the same file page after fork\n");

	munmap(mapping, USER_PAGE_SIZE);
	munmap(poison_region, poison_region_size);
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
	execl("/proc/self/exe", "fork_filemap_ppps", "--run", NULL);
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
