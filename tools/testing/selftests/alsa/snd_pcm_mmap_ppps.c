// SPDX-License-Identifier: GPL-2.0
/*
 * The ALSA PCM mmap fixture rejects, with EINVAL, a 4K compat mapping that
 * crosses the end of its DMA buffer and one that starts on the first process
 * page beyond it.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_ppps.h"

#define MMAP_OFFSET (3 * PROCESS_PAGE_SIZE)
#define MMAP_SIZE (2 * PROCESS_PAGE_SIZE)
#define TAIL_OFFSET (2 * PROCESS_PAGE_SIZE)

static int run_test(void)
{
	void *mapping;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(3);
	fd = ppps_open_fixture_or_skip("/dev/snd_pcm_mmap_ppps", O_RDWR);
	ksft_test_result(fd >= 0, "open the PCM mmap test device\n");

	errno = 0;
	mapping = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, MMAP_OFFSET);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a PCM mapping that crosses the DMA buffer end\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, MMAP_SIZE);

	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, TAIL_OFFSET);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject the first process page beyond the DMA buffer (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
