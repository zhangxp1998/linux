// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define DEST_COUNT 128

static int run_test(void)
{
	struct file_dedupe_range *range;
	int saved_errno;
	int fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	range = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (range == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	if (mprotect((char *)range + USER_PAGE_SIZE, USER_PAGE_SIZE,
		     PROT_NONE))
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));
	range->dest_count = DEST_COUNT;

	fd = open("/tmp/file-dedupe-range-size", O_CREAT | O_RDWR | O_TRUNC,
		  0600);
	if (fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));

	errno = 0;
	ret = ioctl(fd, FIDEDUPERANGE, range);
	saved_errno = errno;
	ksft_print_msg("FIDEDUPERANGE returned %d, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == ENOMEM,
			 "bound dedupe arguments to the process page\n");

	close(fd);
	unlink("/tmp/file-dedupe-range-size");
	munmap(range, NATIVE_PAGE_SIZE);
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
	execl("/proc/self/exe", "file_dedupe_range_size_ppps", "--run",
	      NULL);
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
