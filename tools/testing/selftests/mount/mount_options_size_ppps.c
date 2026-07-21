// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL

static int run_test(void)
{
	char mountpoint[] = "/tmp/mount-options-size-XXXXXX";
	char *options;
	int saved_errno;
	int ret;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (geteuid())
		ksft_exit_skip("mount test requires root\n");

	options = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (options == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	/*
	 * Empty comma-separated parameters are accepted by tmpfs.  A 4K
	 * process may supply at most the first page: mount(2) must terminate
	 * it at byte 4095 and must not observe the invalid option in page 2.
	 */
	memset(options, ',', NATIVE_PAGE_SIZE);
	memcpy(options + USER_PAGE_SIZE, "ppps_invalid_option=1",
	       sizeof("ppps_invalid_option=1"));

	if (!mkdtemp(mountpoint))
		ksft_exit_fail_msg("mkdtemp failed: %s\n", strerror(errno));

	errno = 0;
	ret = mount("tmpfs", mountpoint, "tmpfs", MS_NOSUID | MS_NODEV,
		    options);
	saved_errno = errno;
	ksft_print_msg("mount returned %d, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == 0,
			 "bound mount options to the process page\n");

	ret = ret ? -1 : umount(mountpoint);
	ksft_test_result(ret == 0, "unmount the test filesystem\n");
	rmdir(mountpoint);
	munmap(options, NATIVE_PAGE_SIZE);
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
	execl("/proc/self/exe", "mount_options_size_ppps", "--run", NULL);
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
