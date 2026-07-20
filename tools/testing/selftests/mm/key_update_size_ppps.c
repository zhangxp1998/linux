// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/keyctl.h>
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

static long add_user_key(const void *payload, size_t size)
{
	return syscall(__NR_add_key, "user", "ppps-key-update", payload, size,
		       KEY_SPEC_PROCESS_KEYRING);
}

static long update_key(long key, const void *payload, size_t size)
{
	return syscall(__NR_keyctl, KEYCTL_UPDATE, key, payload, size);
}

static int run_test(void)
{
	unsigned char *payload;
	int saved_errno;
	long key;
	long ret;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	key = add_user_key("x", 1);
	ksft_test_result(key >= 0, "create a baseline user key\n");
	ret = key >= 0 ? update_key(key, "y", 1) : -1;
	ksft_test_result(ret == 0, "update a baseline user key payload\n");

	payload = calloc(1, EXTENDED_SIZE);
	if (!payload)
		ksft_exit_fail_msg("allocation failed\n");
	errno = 0;
	ret = key >= 0 ? update_key(key, payload, EXTENDED_SIZE) : -1;
	saved_errno = errno;
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound key payload size to the process page\n");
	free(payload);
	if (key >= 0)
		syscall(__NR_keyctl, KEYCTL_UNLINK, key,
			KEY_SPEC_PROCESS_KEYRING);
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
	execl("/proc/self/exe", "key_update_size_ppps", "--run", NULL);
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
