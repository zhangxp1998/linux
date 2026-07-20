// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int create_map(union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, BPF_MAP_CREATE, attr, size);
}

static int run_test(void)
{
	union bpf_attr baseline = {
		.map_type = BPF_MAP_TYPE_ARRAY,
		.key_size = sizeof(unsigned int),
		.value_size = sizeof(unsigned int),
		.max_entries = 1,
	};
	union bpf_attr *extended;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = create_map(&baseline, sizeof(baseline));
	ksft_test_result(fd >= 0, "create a baseline BPF array map\n");
	if (fd >= 0)
		close(fd);

	extended = calloc(1, EXTENDED_SIZE);
	ksft_test_result(extended,
			 "allocate a zero-extended BPF attribute\n");
	if (!extended)
		ksft_exit_fail_msg("allocation failed\n");
	memcpy(extended, &baseline, sizeof(baseline));
	errno = 0;
	fd = create_map(extended, EXTENDED_SIZE);
	saved_errno = errno;
	ksft_test_result(fd == -1 && saved_errno == E2BIG,
			 "bound attribute size to the process page\n");
	if (fd >= 0)
		close(fd);
	free(extended);
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
	execl("/proc/self/exe", "bpf_attr_size_ppps", "--run", NULL);
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
