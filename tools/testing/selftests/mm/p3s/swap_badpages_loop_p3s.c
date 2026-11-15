// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include "p3s_harness.h"

/* Only formats a new, test-owned file, never an existing block device. */
static unsigned int header_size;
static unsigned int badpages = 3;
static bool swapped;

TEST(valid_block_swap_header)
{
	unsigned char header[NATIVE_PAGE_SIZE] = {};
	char path[] = "/data/local/tmp/p3s-badpages-XXXXXX", dev[64];
	const unsigned int bytes = 32 * 1024 * 1024;
	unsigned int i;
	int ctl, loop, fd, index, ret, error;
	int swapoff_ret = 0, swapoff_error = 0;
	struct loop_info64 info = { .lo_flags = LO_FLAGS_AUTOCLEAR };

	fd = mkstemp(path);
	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		ctl = open("/dev/block/loop-control", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ctl < 0)
		SKIP(return, "test file or loop-control unavailable: %s", strerror(errno));
	unlink(path);
	ASSERT_EQ(0, ftruncate(fd, bytes));
	for (i = 0; i < 3 + badpages; i++) {
		uint32_t value;
		size_t offset;

		if (i < 3) {
			offset = 1024 + 4 * i;
			value = i == 0 ? 1 : i == 1 ? bytes / header_size - 1 : badpages;
		} else {
			offset = 1536 + 4 * (i - 3);
			value = (i - 2) * (getpagesize() / header_size);
		}
		if (swapped)
			value = __builtin_bswap32(value);
		memcpy(header + offset, &value, sizeof(value));
	}
	memcpy(header + header_size - 10, "SWAPSPACE2", 10);
	ASSERT_EQ(sizeof(header), pwrite(fd, header, sizeof(header), 0));
	ASSERT_EQ(0, fsync(fd));
	index = ioctl(ctl, LOOP_CTL_GET_FREE);
	if (index < 0)
		SKIP(return, "no free loop device");
	snprintf(dev, sizeof(dev), "/dev/block/loop%d", index);
	loop = open(dev, O_RDWR | O_CLOEXEC);
	/* Android ueventd creates a newly allocated loop node asynchronously. */
	for (i = 0; loop < 0 && errno == ENOENT && i < 20; i++) {
		usleep(50000);
		loop = open(dev, O_RDWR | O_CLOEXEC);
	}
	if (loop < 0) {
		snprintf(dev, sizeof(dev), "/dev/loop%d", index);
		loop = open(dev, O_RDWR | O_CLOEXEC);
	}
	if (loop < 0 || ioctl(loop, LOOP_SET_FD, fd))
		SKIP(return, "cannot attach owned loop: %s", strerror(errno));
	ret = ioctl(loop, LOOP_SET_STATUS64, &info);
	if (ret) {
		error = errno;
		ioctl(loop, LOOP_CLR_FD, 0);
		close(loop); close(ctl); close(fd);
		ASSERT_EQ(0, ret) TH_LOG("loop flags: %s", strerror(error));
	}
	TH_LOG("swapon owned %s header=%u badpages=%u swab=%u",
	       dev, header_size, badpages, swapped);
	ret = syscall(SYS_swapon, dev, 0);
	error = errno;
	if (!ret) {
		swapoff_ret = syscall(SYS_swapoff, dev);
		swapoff_error = errno;
	}
	ioctl(loop, LOOP_CLR_FD, 0);
	close(loop); close(ctl); close(fd);
	EXPECT_EQ(0, ret) TH_LOG("swapon ret=%d errno=%d", ret, error);
	EXPECT_EQ(0, swapoff_ret) TH_LOG("swapoff ret=%d errno=%d",
				      swapoff_ret, swapoff_error);
}

int main(int argc, char **argv)
{
	header_size = getpagesize();
	if (argc > 1 && !strcmp(argv[1], "4k"))
		header_size = PROCESS_PAGE_SIZE;
	if (argc > 2 && !strcmp(argv[2], "swab"))
		swapped = true;
	if (argc > 2 && !strcmp(argv[2], "control"))
		badpages = 0;
	p3s_argv0 = argv[0];
	return p3s_harness_run();
}
