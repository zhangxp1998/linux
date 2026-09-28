// SPDX-License-Identifier: GPL-2.0
/* Run the generic PAGEMAP_SCAN regression suite under 4K P3S geometry. */
#define _GNU_SOURCE

#include <limits.h>

#define P3S_HARNESS_ROLE pagemap_role
#include "p3s_harness.h"

static int run_test(void)
{
	char executable[PATH_MAX];
	char *name;
	ssize_t length;

	length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
	if (length < 0 || length >= (ssize_t)sizeof(executable) - 1)
		_exit(KSFT_FAIL);
	executable[length] = '\0';
	name = strrchr(executable, '/');
	if (!name)
		_exit(KSFT_FAIL);
	strcpy(name + 1, "pagemap_ioctl");
	execl(executable, executable, NULL);
	_exit(errno == ENOENT ? KSFT_SKIP : KSFT_FAIL);
}

static int pagemap_role(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "--pagemap-ioctl"))
		return -1;
	p3s_require_4kb();
	return run_test();
}

TEST(pagemap_scan_compat_suite)
{
	int result = p3s_run_child(true, "--pagemap-ioctl", NULL);

	if (result == KSFT_SKIP)
		SKIP(return, "generic pagemap test is unavailable");
	ASSERT_EQ(KSFT_PASS, result);
}

P3S_HARNESS_4KB_MAIN
