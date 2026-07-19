// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define MMAP_OFFSET (3 * USER_PAGE_SIZE)
#define MMAP_SIZE (2 * USER_PAGE_SIZE)
#define TAIL_OFFSET (2 * USER_PAGE_SIZE)

static int run_test(void)
{
	void *mapping;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/dev/snd_pcm_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the PCM mmap test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	errno = 0;
	mapping = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, MMAP_OFFSET);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a PCM mapping that crosses the DMA buffer end\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, MMAP_SIZE);

	errno = 0;
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, TAIL_OFFSET);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject the first process page beyond the DMA buffer (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "snd_pcm_mmap_ppps", "--run", NULL);
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
