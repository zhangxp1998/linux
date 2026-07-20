// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define TEST_IOCTL_SIZE	4096U

static int create_listener(void)
{
	struct sock_filter insns[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getppid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = ARRAY_SIZE(insns),
		.filter = insns,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	return syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

static int run_test(void)
{
	unsigned char arg[TEST_IOCTL_SIZE] = {};
	struct seccomp_notif_addfd *addfd = (void *)arg;
	unsigned long cmd;
	int listener;
	int ret;
	int saved_errno;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	listener = create_listener();
	ksft_test_result(listener >= 0,
			 "create a seccomp user-notification listener\n");
	if (listener < 0)
		ksft_exit_fail_msg("listener creation failed: %s\n",
				   strerror(errno));

	cmd = _IOC(_IOC_WRITE, SECCOMP_IOC_MAGIC, 3, TEST_IOCTL_SIZE);
	ksft_test_result(_IOC_SIZE(cmd) == TEST_IOCTL_SIZE,
			 "encode a 4096-byte ADDFD ioctl\n");
	addfd->srcfd = UINT_MAX;
	errno = 0;
	ret = ioctl(listener, cmd, arg);
	saved_errno = errno;
	ksft_test_result(ret == -1, "reject an oversized ADDFD request\n");
	ksft_test_result(saved_errno == EINVAL,
			 "bound ADDFD size to the process page\n");
	close(listener);
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
	execl("/proc/self/exe", "seccomp_addfd_ppps", "--run", NULL);
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
