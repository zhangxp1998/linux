// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL

static int run_test(void)
{
	char *policy;
	int fd;
	int ret;
	int saved_errno;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	ksft_test_result(fd >= 0, "create an IPv4 socket\n");
	if (fd < 0)
		ksft_exit_fail_msg("socket failed: %s\n", strerror(errno));
	policy = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(policy != MAP_FAILED, "map two process pages\n");
	if (policy == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(policy, 0, USER_PAGE_SIZE);
	ret = mprotect(policy + USER_PAGE_SIZE, USER_PAGE_SIZE, PROT_NONE);
	ksft_test_result(ret == 0, "protect the following process page\n");
	if (ret)
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));
	errno = 0;
	ret = setsockopt(fd, IPPROTO_IP, IP_XFRM_POLICY, policy,
			 USER_PAGE_SIZE + 1);
	saved_errno = errno;
	ksft_print_msg("IP_XFRM_POLICY returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == EMSGSIZE,
			 "bound XFRM policy input to the process page\n");
	munmap(policy, 2 * USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "xfrm_policy_size_ppps", "--run", NULL);
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
