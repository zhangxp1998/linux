// SPDX-License-Identifier: GPL-2.0
/*
 * The ALSA PCM mmap fixture rejects, with EINVAL, a 4K compat mapping that
 * crosses the end of its DMA buffer and one that starts on the first process
 * page beyond it.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define MMAP_OFFSET (3 * USER_PAGE_SIZE)
#define MMAP_SIZE (2 * USER_PAGE_SIZE)

static int run_test(void)
{
	void *mapping;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/dev/snd_pcm_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the PCM mmap test device\n");

	errno = 0;
	mapping = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, MMAP_OFFSET);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a PCM mapping that crosses the DMA buffer end\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, MMAP_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
