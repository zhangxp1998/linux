// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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

#define USER_PAGE_SIZE 4096UL
#define STRICT_MAP_SIZE (4UL * 1024 * 1024)

static void write_sysctl(const char *path, unsigned long value)
{
	char buffer[32];
	ssize_t length;
	int fd;

	length = snprintf(buffer, sizeof(buffer), "%lu\n", value);
	if (length <= 0 || (size_t)length >= sizeof(buffer))
		ksft_exit_fail_msg("format %s value failed\n", path);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", path,
				   strerror(errno));
	if (write(fd, buffer, length) != length)
		ksft_exit_fail_msg("write %s failed: %s\n", path,
				   strerror(errno));
	if (close(fd))
		ksft_exit_fail_msg("close %s failed: %s\n", path,
				   strerror(errno));
}

static unsigned long committed_kb(void)
{
	unsigned long value = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		ksft_exit_fail_msg("open /proc/meminfo failed: %s\n",
				   strerror(errno));
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "Committed_AS: %lu kB", &value) == 1)
			break;
	}
	free(line);
	fclose(meminfo);
	if (!value)
		ksft_exit_fail_msg("read Committed_AS failed\n");
	return value;
}

static int run_test(void)
{
	unsigned long before;
	unsigned long after_map;
	unsigned long after_expand;
	unsigned long after_unmap;
	long initial_charge;
	long expansion_charge;
	long uncharge;
	void *mapping;
	void *expanded;
	int strict_errno;
	int ret;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	before = committed_kb();
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map one accountable process page\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	after_map = committed_kb();
	initial_charge = after_map - before;
	ksft_test_result(initial_charge == USER_PAGE_SIZE / 1024,
			 "charge exactly one 4K process page\n");

	expanded = mremap(mapping, USER_PAGE_SIZE, 2 * USER_PAGE_SIZE,
			  MREMAP_MAYMOVE);
	ksft_test_result(expanded != MAP_FAILED,
			 "expand the mapping by one process page\n");
	if (expanded == MAP_FAILED)
		ksft_exit_fail_msg("mremap failed: %s\n", strerror(errno));
	after_expand = committed_kb();
	expansion_charge = after_expand - after_map;
	ksft_test_result(expansion_charge == USER_PAGE_SIZE / 1024,
			 "charge exactly one 4K mremap expansion page\n");
	ksft_print_msg("Committed_AS before=%lu mapped=%lu expanded=%lu kB; "
		       "initial=%ld expansion=%ld kB\n",
		       before, after_map, after_expand, initial_charge,
		       expansion_charge);

	ret = munmap(expanded, 2 * USER_PAGE_SIZE);
	after_unmap = committed_kb();
	ksft_test_result(ret == 0, "unmap the expanded mapping\n");
	if (ret)
		ksft_exit_fail_msg("munmap failed: %s\n", strerror(errno));
	uncharge = after_expand - after_unmap;
	ksft_test_result(uncharge == 2 * USER_PAGE_SIZE / 1024,
			 "uncharge both 4K process pages\n");

	write_sysctl("/proc/sys/vm/user_reserve_kbytes", 0);
	write_sysctl("/proc/sys/vm/admin_reserve_kbytes", 0);
	before = committed_kb();
	write_sysctl("/proc/sys/vm/overcommit_kbytes",
		     before + 2 * STRICT_MAP_SIZE / 1024);
	write_sysctl("/proc/sys/vm/overcommit_memory", 2);
	mapping = mmap(NULL, STRICT_MAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "strict overcommit admits one 4M mapping\n");
	errno = 0;
	expanded = mmap(NULL, STRICT_MAP_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	strict_errno = errno;
	ksft_test_result(expanded == MAP_FAILED && strict_errno == ENOMEM,
			 "strict overcommit rejects the 8M boundary\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, STRICT_MAP_SIZE);
	if (expanded != MAP_FAILED)
		munmap(expanded, STRICT_MAP_SIZE);
	write_sysctl("/proc/sys/vm/overcommit_memory", 0);
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
	execl("/proc/self/exe", "mremap_commit_ppps", "--run", NULL);
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
