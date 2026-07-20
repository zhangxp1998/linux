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

static long create_keyring(void)
{
	return syscall(__NR_add_key, "keyring", "ppps-restrict", NULL, 0,
		       KEY_SPEC_PROCESS_KEYRING);
}

static long restrict_keyring(long keyring, const char *restriction)
{
	return syscall(__NR_keyctl, KEYCTL_RESTRICT_KEYRING, keyring,
		       "ppps-no-such-key-type", restriction);
}

static int run_test(void)
{
	char *restriction;
	int saved_errno;
	long keyring;
	long ret;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	keyring = create_keyring();
	ksft_test_result(keyring >= 0, "create a baseline keyring\n");
	errno = 0;
	ret = keyring >= 0 ? restrict_keyring(keyring, "x") : -1;
	ksft_test_result(ret == -1 && errno == ENOKEY,
			 "reject an unknown restriction key type\n");

	restriction = malloc(EXTENDED_SIZE);
	ksft_test_result(restriction,
			 "allocate a 4096-character restriction\n");
	if (!restriction)
		ksft_exit_fail_msg("allocation failed\n");
	memset(restriction, 'x', USER_PAGE_SIZE);
	restriction[USER_PAGE_SIZE] = '\0';
	errno = 0;
	ret = keyring >= 0 ? restrict_keyring(keyring, restriction) : -1;
	saved_errno = errno;
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound keyring restriction to the process page\n");
	free(restriction);
	if (keyring >= 0)
		syscall(__NR_keyctl, KEYCTL_UNLINK, keyring,
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
	execl("/proc/self/exe", "key_restrict_size_ppps", "--run", NULL);
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
