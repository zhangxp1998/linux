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

#include <linux/dma-buf.h>
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
	struct {
		__u32 flags;
		__u32 count;
		struct udmabuf_create_item list[2];
	} create_list = {};
	struct udmabuf_create create = {};
	struct dma_buf_sync sync = {};
	unsigned char *list_mapping;
	unsigned char *subpage_mapping;
	unsigned char *backing;
	unsigned char *mapping;
	void *helper_mapping;
	bool contents_ok = true;
	bool writeback_ok;
	off_t exported_size;
	size_t offset;
	int memfd;
	int devfd;
	int buf_fd;
	int list_fd;
	int subpage_fd;
	int helper_fd;

	ksft_print_header();
	ksft_set_plan(17);
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

	helper_fd = open("/dev/dmabuf_mmap_ppps", O_RDWR | O_CLOEXEC);
	if (helper_fd < 0)
		ksft_exit_fail_msg("open dma_buf_mmap helper failed: %s\n",
				   strerror(errno));
	helper_mapping = mmap(NULL, BUFFER_SIZE, PROT_NONE, MAP_SHARED,
			      helper_fd, USER_PAGE_SIZE);
	ksft_test_result(helper_mapping != MAP_FAILED,
			 "map a complete buffer through dma_buf_mmap()\n");
	if (helper_mapping != MAP_FAILED)
		munmap(helper_mapping, BUFFER_SIZE);
	close(helper_fd);

	create.offset = USER_PAGE_SIZE;
	create.size = 2 * USER_PAGE_SIZE;
	subpage_fd = ioctl(devfd, UDMABUF_CREATE, &create);
	ksft_test_result(subpage_fd >= 0,
			 "create a dma-buf from a 4K-aligned subrange\n");
	if (subpage_fd < 0) {
		ksft_test_result_fail("report the exact subrange size\n");
		ksft_test_result_fail("map the 4K-aligned subrange\n");
		ksft_test_result_fail("map the requested memfd bytes\n");
		ksft_test_result_fail("write back through the subrange mapping\n");
		ksft_test_result_fail("begin CPU access to the subrange\n");
		ksft_test_result_fail("end CPU access to the subrange\n");
		goto test_list;
	}

	exported_size = lseek(subpage_fd, 0, SEEK_END);
	ksft_test_result(exported_size == 2 * USER_PAGE_SIZE,
			 "report the exact subrange size\n");
	subpage_mapping = mmap(NULL, 2 * USER_PAGE_SIZE,
			       PROT_READ | PROT_WRITE, MAP_SHARED,
			       subpage_fd, 0);
	ksft_test_result(subpage_mapping != MAP_FAILED,
			 "map the 4K-aligned subrange\n");
	if (subpage_mapping == MAP_FAILED) {
		ksft_test_result_fail("map the requested memfd bytes\n");
		ksft_test_result_fail("write back through the subrange mapping\n");
		ksft_test_result_fail("begin CPU access to the subrange\n");
		ksft_test_result_fail("end CPU access to the subrange\n");
		close(subpage_fd);
		goto test_list;
	}

	contents_ok = subpage_mapping[0] == 0x32 &&
		      subpage_mapping[USER_PAGE_SIZE] == 0x33;
	ksft_test_result(contents_ok, "map the requested memfd bytes\n");
	subpage_mapping[0] = 0xa1;
	subpage_mapping[USER_PAGE_SIZE] = 0xa2;
	writeback_ok = backing[USER_PAGE_SIZE] == 0xa1 &&
		       backing[2 * USER_PAGE_SIZE] == 0xa2;
	ksft_test_result(writeback_ok,
			 "write back through the subrange mapping\n");
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
	ksft_test_result(ioctl(subpage_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
			 "begin CPU access to the subrange\n");
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	ksft_test_result(ioctl(subpage_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
			 "end CPU access to the subrange\n");
	munmap(subpage_mapping, 2 * USER_PAGE_SIZE);
	close(subpage_fd);

test_list:
	create_list.count = 2;
	create_list.list[0].memfd = memfd;
	create_list.list[0].size = USER_PAGE_SIZE;
	create_list.list[1].memfd = memfd;
	create_list.list[1].offset = 3 * USER_PAGE_SIZE;
	create_list.list[1].size = USER_PAGE_SIZE;
	list_fd = ioctl(devfd, UDMABUF_CREATE_LIST, &create_list);
	ksft_test_result(list_fd >= 0,
			 "create a dma-buf from discontiguous 4K slices\n");
	if (list_fd < 0) {
		ksft_test_result_fail("report the exact slice-list size\n");
		ksft_test_result_fail("map the discontiguous slice list\n");
		ksft_test_result_fail("preserve the slice-list order\n");
		goto out;
	}

	exported_size = lseek(list_fd, 0, SEEK_END);
	ksft_test_result(exported_size == 2 * USER_PAGE_SIZE,
			 "report the exact slice-list size\n");
	list_mapping = mmap(NULL, 2 * USER_PAGE_SIZE,
			    PROT_READ | PROT_WRITE, MAP_SHARED, list_fd, 0);
	ksft_test_result(list_mapping != MAP_FAILED,
			 "map the discontiguous slice list\n");
	if (list_mapping == MAP_FAILED) {
		ksft_test_result_fail("preserve the slice-list order\n");
		close(list_fd);
		goto out;
	}
	contents_ok = list_mapping[0] == 0x31 &&
		      list_mapping[USER_PAGE_SIZE] == 0x34;
	ksft_test_result(contents_ok, "preserve the slice-list order\n");
	munmap(list_mapping, 2 * USER_PAGE_SIZE);
	close(list_fd);

out:
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
