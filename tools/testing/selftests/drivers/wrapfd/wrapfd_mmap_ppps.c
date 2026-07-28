// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include <linux/udmabuf.h>
#include <linux/wrapfd.h>

#include "../../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define BUFFER_SIZE	(4 * USER_PAGE_SIZE)

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
	ksft_set_plan(13);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	ksft_test_result(udmabuf_fd >= 0, "open /dev/udmabuf\n");
	if (udmabuf_fd < 0)
		ksft_exit_fail_msg("open /dev/udmabuf failed: %s\n",
				   strerror(errno));

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
		backing[i * USER_PAGE_SIZE] = markers[i];
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK))
		ksft_exit_fail_msg("seal memfd failed: %s\n", strerror(errno));

	dmabuf_fd = create_dmabuf(udmabuf_fd, memfd);
	ksft_test_result(dmabuf_fd >= 0, "export the memfd as a dma-buf\n");
	if (dmabuf_fd < 0)
		ksft_exit_fail_msg("UDMABUF_CREATE failed: %s\n",
				   strerror(errno));

	wrap_dev_fd = open("/dev/wrapfd", O_RDONLY | O_CLOEXEC);
	ksft_test_result(wrap_dev_fd >= 0, "open /dev/wrapfd\n");
	if (wrap_dev_fd < 0)
		ksft_exit_fail_msg("open /dev/wrapfd failed: %s\n",
				   strerror(errno));

	wrap_fd = create_wrapfd(wrap_dev_fd, dmabuf_fd);
	ksft_test_result(wrap_fd >= 0, "wrap the dma-buf\n");
	if (wrap_fd < 0)
		ksft_exit_fail_msg("WRAPFD_DEV_IOC_WRAP failed: %s\n",
				   strerror(errno));

	second = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		      wrap_fd, USER_PAGE_SIZE);
	ksft_test_result(second != MAP_FAILED,
			 "map the second 4K slice through wrapfd\n");
	if (second == MAP_FAILED)
		ksft_exit_fail_msg("second-slice mmap failed: %s\n",
				   strerror(errno));
	ksft_test_result(second[0] == markers[1],
			 "preserve the second-slice dma-buf offset\n");

	third = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     wrap_fd, 2 * USER_PAGE_SIZE);
	ksft_test_result(third != MAP_FAILED,
			 "map the third 4K slice through wrapfd\n");
	if (third == MAP_FAILED)
		ksft_exit_fail_msg("third-slice mmap failed: %s\n",
				   strerror(errno));
	ksft_test_result(third[0] == markers[2],
			 "preserve the third-slice dma-buf offset\n");

	second[0] = 0xa5;
	ksft_test_result(backing[USER_PAGE_SIZE] == 0xa5,
			 "write back to the selected second slice\n");
	ksft_test_result(backing[0] == markers[0],
			 "leave the first slice unchanged\n");

	munmap(third, USER_PAGE_SIZE);
	munmap(second, USER_PAGE_SIZE);
	close(wrap_fd);
	close(wrap_dev_fd);
	close(dmabuf_fd);
	munmap(backing, BUFFER_SIZE);
	close(memfd);
	close(udmabuf_fd);
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
	execl("/proc/self/exe", "wrapfd_mmap_ppps", "--run", NULL);
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
