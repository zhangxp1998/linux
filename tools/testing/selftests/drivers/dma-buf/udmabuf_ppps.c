// SPDX-License-Identifier: GPL-2.0
/*
 * udmabuf buffers created from 4K-granular memfd ranges export, map and
 * sync at 4K process-page granularity for a compat process, including
 * through the in-kernel dma_buf_mmap() fixture.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/memfd.h>
#include <linux/udmabuf.h>

#include "kselftest_ppps.h"

#define BUFFER_SIZE	(4 * PROCESS_PAGE_SIZE)

static int run_test(void)
{
	struct udmabuf_create create = {};
	unsigned char *backing;
	unsigned char *mapping;
	bool contents_ok = true;
	size_t offset;
	int memfd;
	int devfd;
	int buf_fd;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	devfd = ppps_open_fixture_or_skip("/dev/udmabuf", O_RDWR);
	ksft_test_result(devfd >= 0, "open /dev/udmabuf\n");

	memfd = memfd_create("udmabuf-ppps", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, BUFFER_SIZE))
		ksft_exit_fail_msg("create memfd failed: %s\n", strerror(errno));
	backing = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	if (backing == MAP_FAILED)
		ksft_exit_fail_msg("map backing memfd failed: %s\n",
				   strerror(errno));
	for (offset = 0; offset < BUFFER_SIZE; offset += PROCESS_PAGE_SIZE)
		backing[offset] = 0x31 + offset / PROCESS_PAGE_SIZE;
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK))
		ksft_exit_fail_msg("seal memfd failed: %s\n", strerror(errno));

	create.memfd = memfd;
	create.offset = 0;
	create.size = BUFFER_SIZE;
	buf_fd = ioctl(devfd, UDMABUF_CREATE, &create);
	ksft_test_result(buf_fd >= 0, "create a four-process-page dma-buf\n");
	if (buf_fd < 0)
		ksft_exit_fail_msg("UDMABUF_CREATE failed: %s\n", strerror(errno));

	mapping = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       buf_fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the complete dma-buf in process page units\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("dma-buf mmap failed: %s\n", strerror(errno));

	for (offset = 0; offset < BUFFER_SIZE; offset += PROCESS_PAGE_SIZE) {
		if (mapping[offset] != 0x31 + offset / PROCESS_PAGE_SIZE) {
			contents_ok = false;
			break;
		}
	}
	ksft_test_result(contents_ok,
			 "preserve all four process-page slices\n");

	munmap(mapping, BUFFER_SIZE);
	munmap(backing, BUFFER_SIZE);
	close(buf_fd);
	close(memfd);
	close(devfd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
