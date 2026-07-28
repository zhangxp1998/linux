// SPDX-License-Identifier: GPL-2.0
/*
 * A udmabuf wrapped through /dev/wrapfd maps its second and third 4K slices
 * at 4K process-page offsets in a compat process, and a write through the
 * wrapped mapping lands only on the selected slice of the backing memfd.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/udmabuf.h>
#include <linux/wrapfd.h>

#include "kselftest_ppps.h"

#define BUFFER_SIZE	(4 * PROCESS_PAGE_SIZE)

static int create_dmabuf(int dev_fd, int memfd)
{
	struct udmabuf_create create = {
		.memfd = memfd,
		.size = BUFFER_SIZE,
	};

	return ioctl(dev_fd, UDMABUF_CREATE, &create);
}

static int create_wrapfd(int dev_fd, int dmabuf_fd)
{
	struct wrapfd_wrap wrap = {
		.fd = dmabuf_fd,
		.prot = PROT_READ | PROT_WRITE,
	};

	return ioctl(dev_fd, WRAPFD_DEV_IOC_WRAP, &wrap);
}

static int run_test(void)
{
	static const unsigned char markers[] = { 0x11, 0x22, 0x33, 0x44 };
	unsigned char *second;
	unsigned char *third;
	unsigned char *backing;
	int wrap_dev_fd;
	int udmabuf_fd;
	int dmabuf_fd;
	int wrap_fd;
	int memfd;
	size_t i;

	ksft_print_header();
	ksft_set_plan(12);

	udmabuf_fd = ppps_open_fixture_or_skip("/dev/udmabuf", O_RDWR);
	ksft_test_result(udmabuf_fd >= 0, "open /dev/udmabuf\n");

	memfd = memfd_create("wrapfd-mmap-ppps",
			     MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (memfd >= 0 && ftruncate(memfd, BUFFER_SIZE)) {
		close(memfd);
		memfd = -1;
	}
	ksft_test_result(memfd >= 0, "create the 16K backing memfd\n");
	if (memfd < 0)
		ksft_exit_fail_msg("create memfd failed: %s\n",
				   strerror(errno));

	backing = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	ksft_test_result(backing != MAP_FAILED, "map the backing memfd\n");
	if (backing == MAP_FAILED)
		ksft_exit_fail_msg("map backing memfd failed: %s\n",
				   strerror(errno));
	for (i = 0; i < sizeof(markers); i++)
		backing[i * PROCESS_PAGE_SIZE] = markers[i];
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK))
		ksft_exit_fail_msg("seal memfd failed: %s\n", strerror(errno));

	dmabuf_fd = create_dmabuf(udmabuf_fd, memfd);
	ksft_test_result(dmabuf_fd >= 0, "export the memfd as a dma-buf\n");
	if (dmabuf_fd < 0)
		ksft_exit_fail_msg("UDMABUF_CREATE failed: %s\n",
				   strerror(errno));

	wrap_dev_fd = ppps_open_fixture_or_skip("/dev/wrapfd", O_RDONLY);
	ksft_test_result(wrap_dev_fd >= 0, "open /dev/wrapfd\n");

	wrap_fd = create_wrapfd(wrap_dev_fd, dmabuf_fd);
	ksft_test_result(wrap_fd >= 0, "wrap the dma-buf\n");
	if (wrap_fd < 0)
		ksft_exit_fail_msg("WRAPFD_DEV_IOC_WRAP failed: %s\n",
				   strerror(errno));

	second = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		      wrap_fd, PROCESS_PAGE_SIZE);
	ksft_test_result(second != MAP_FAILED,
			 "map the second 4K slice through wrapfd\n");
	if (second == MAP_FAILED)
		ksft_exit_fail_msg("second-slice mmap failed: %s\n",
				   strerror(errno));
	ksft_test_result(second[0] == markers[1],
			 "preserve the second-slice dma-buf offset\n");

	third = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     wrap_fd, 2 * PROCESS_PAGE_SIZE);
	ksft_test_result(third != MAP_FAILED,
			 "map the third 4K slice through wrapfd\n");
	if (third == MAP_FAILED)
		ksft_exit_fail_msg("third-slice mmap failed: %s\n",
				   strerror(errno));
	ksft_test_result(third[0] == markers[2],
			 "preserve the third-slice dma-buf offset\n");

	second[0] = 0xa5;
	ksft_test_result(backing[PROCESS_PAGE_SIZE] == 0xa5,
			 "write back to the selected second slice\n");
	ksft_test_result(backing[0] == markers[0],
			 "leave the first slice unchanged\n");

	munmap(third, PROCESS_PAGE_SIZE);
	munmap(second, PROCESS_PAGE_SIZE);
	close(wrap_fd);
	close(wrap_dev_fd);
	close(dmabuf_fd);
	munmap(backing, BUFFER_SIZE);
	close(memfd);
	close(udmabuf_fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
