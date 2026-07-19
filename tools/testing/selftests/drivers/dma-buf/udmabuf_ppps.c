// SPDX-License-Identifier: GPL-2.0
/*
 * udmabuf buffers created from 4K-granular memfd ranges export, map and
 * sync at 4K process-page granularity for a compat process, including
 * through the in-kernel dma_buf_mmap() fixture.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/dma-buf.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>

#include "kselftest_ppps.h"

#define BUFFER_SIZE	(4 * PROCESS_PAGE_SIZE)

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
	ksft_set_plan(16);

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

	helper_fd = ppps_open_fixture_or_skip("/dev/dmabuf_mmap_ppps", O_RDWR);
	helper_mapping = mmap(NULL, BUFFER_SIZE, PROT_NONE, MAP_SHARED,
			      helper_fd, PROCESS_PAGE_SIZE);
	ksft_test_result(helper_mapping != MAP_FAILED,
			 "map a complete buffer through dma_buf_mmap()\n");
	if (helper_mapping != MAP_FAILED)
		munmap(helper_mapping, BUFFER_SIZE);
	close(helper_fd);

	create.offset = PROCESS_PAGE_SIZE;
	create.size = 2 * PROCESS_PAGE_SIZE;
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
	ksft_test_result(exported_size == 2 * PROCESS_PAGE_SIZE,
			 "report the exact subrange size\n");
	subpage_mapping = mmap(NULL, 2 * PROCESS_PAGE_SIZE,
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
		      subpage_mapping[PROCESS_PAGE_SIZE] == 0x33;
	ksft_test_result(contents_ok, "map the requested memfd bytes\n");
	subpage_mapping[0] = 0xa1;
	subpage_mapping[PROCESS_PAGE_SIZE] = 0xa2;
	writeback_ok = backing[PROCESS_PAGE_SIZE] == 0xa1 &&
		       backing[2 * PROCESS_PAGE_SIZE] == 0xa2;
	ksft_test_result(writeback_ok,
			 "write back through the subrange mapping\n");
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
	ksft_test_result(ioctl(subpage_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
			 "begin CPU access to the subrange\n");
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	ksft_test_result(ioctl(subpage_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
			 "end CPU access to the subrange\n");
	munmap(subpage_mapping, 2 * PROCESS_PAGE_SIZE);
	close(subpage_fd);

test_list:
	create_list.count = 2;
	create_list.list[0].memfd = memfd;
	create_list.list[0].size = PROCESS_PAGE_SIZE;
	create_list.list[1].memfd = memfd;
	create_list.list[1].offset = 3 * PROCESS_PAGE_SIZE;
	create_list.list[1].size = PROCESS_PAGE_SIZE;
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
	ksft_test_result(exported_size == 2 * PROCESS_PAGE_SIZE,
			 "report the exact slice-list size\n");
	list_mapping = mmap(NULL, 2 * PROCESS_PAGE_SIZE,
			    PROT_READ | PROT_WRITE, MAP_SHARED, list_fd, 0);
	ksft_test_result(list_mapping != MAP_FAILED,
			 "map the discontiguous slice list\n");
	if (list_mapping == MAP_FAILED) {
		ksft_test_result_fail("preserve the slice-list order\n");
		close(list_fd);
		goto out;
	}
	contents_ok = list_mapping[0] == 0x31 &&
		      list_mapping[PROCESS_PAGE_SIZE] == 0x34;
	ksft_test_result(contents_ok, "preserve the slice-list order\n");
	munmap(list_mapping, 2 * PROCESS_PAGE_SIZE);
	close(list_fd);

out:
	munmap(mapping, BUFFER_SIZE);
	munmap(backing, BUFFER_SIZE);
	close(buf_fd);
	close(memfd);
	close(devfd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
