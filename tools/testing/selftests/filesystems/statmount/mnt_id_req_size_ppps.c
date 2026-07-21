// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/mount.h>
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

#ifndef __NR_statmount
#define __NR_statmount 457
#endif

#ifndef __NR_listmount
#define __NR_listmount 458
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

static int run_test(void)
{
	struct mnt_id_req *req;
	struct statmount statbuf;
	uint64_t ids[16];
	int saved_errno;
	long ret;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	req = calloc(1, EXTENDED_SIZE);
	ksft_test_result(req, "allocate zero-extended mount request\n");
	if (!req)
		ksft_exit_fail_msg("allocation failed\n");

	req->size = MNT_ID_REQ_SIZE_VER0;
	req->mnt_id = LSMT_ROOT;
	ret = syscall(__NR_listmount, req, ids, 16, 0);
	ksft_test_result(ret > 0, "list mounts with a baseline request\n");
	if (ret <= 0)
		ksft_exit_fail_msg("baseline listmount failed: %s\n",
				   strerror(errno));

	memset(req, 0, EXTENDED_SIZE);
	req->size = MNT_ID_REQ_SIZE_VER0;
	req->mnt_id = ids[0];
	req->param = STATMOUNT_MNT_BASIC;
	ret = syscall(__NR_statmount, req, &statbuf, sizeof(statbuf), 0);
	ksft_test_result(ret == 0, "stat a mount with a baseline request\n");

	memset(req, 0, EXTENDED_SIZE);
	req->size = EXTENDED_SIZE;
	req->mnt_id = LSMT_ROOT;
	errno = 0;
	ret = syscall(__NR_listmount, req, ids, 16, 0);
	saved_errno = errno;
	ksft_print_msg("extended listmount returned %ld, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound listmount request to the process page\n");

	memset(req, 0, EXTENDED_SIZE);
	req->size = EXTENDED_SIZE;
	req->mnt_id = ids[0];
	req->param = STATMOUNT_MNT_BASIC;
	errno = 0;
	ret = syscall(__NR_statmount, req, &statbuf, sizeof(statbuf), 0);
	saved_errno = errno;
	ksft_print_msg("extended statmount returned %ld, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == E2BIG,
			 "bound statmount request to the process page\n");
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
	execl("/proc/self/exe", "mnt_id_req_size_ppps", "--run", NULL);
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
