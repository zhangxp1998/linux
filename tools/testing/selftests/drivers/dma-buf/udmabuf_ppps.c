// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include <linux/memfd.h>
#include <linux/udmabuf.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define BUFFER_SIZE	(4 * USER_PAGE_SIZE)

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

	devfd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	ksft_test_result(devfd >= 0, "open /dev/udmabuf\n");
	if (devfd < 0)
		ksft_exit_fail_msg("open /dev/udmabuf failed: %s\n",
				   strerror(errno));

	memfd = memfd_create("udmabuf-ppps", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, BUFFER_SIZE))
		ksft_exit_fail_msg("create memfd failed: %s\n", strerror(errno));
	backing = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	if (backing == MAP_FAILED)
		ksft_exit_fail_msg("map backing memfd failed: %s\n",
				   strerror(errno));
	for (offset = 0; offset < BUFFER_SIZE; offset += USER_PAGE_SIZE)
		backing[offset] = 0x31 + offset / USER_PAGE_SIZE;
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

	for (offset = 0; offset < BUFFER_SIZE; offset += USER_PAGE_SIZE) {
		if (mapping[offset] != 0x31 + offset / USER_PAGE_SIZE) {
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

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "udmabuf_ppps", "--run", NULL);
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
