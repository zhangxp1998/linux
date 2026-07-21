// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/nsfs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef __NR_listns
#define __NR_listns 470
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct ns_id_req *req;
	uint64_t ids[32];
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	req = calloc(1, EXTENDED_SIZE);
	ksft_test_result(req, "allocate zero-extended namespace request\n");
	if (!req)
		ksft_exit_fail_msg("allocation failed\n");

	req->size = NS_ID_REQ_SIZE_VER0;
	ret = syscall(__NR_listns, req, ids, 32, 0);
	ksft_test_result(ret > 0, "list namespaces with a baseline request\n");
	if (ret <= 0)
		ksft_exit_fail_msg("baseline listns failed: %s\n",
				   strerror(errno));

	memset(req, 0, EXTENDED_SIZE);
	req->size = EXTENDED_SIZE;
	errno = 0;
	ret = syscall(__NR_listns, req, ids, 32, 0);
	saved_errno = errno;
	ksft_print_msg("extended listns returned %ld, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound namespace requests to the process page\n");
	free(req);
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
	execl("/proc/self/exe", "listns_size_ppps", "--run", NULL);
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
