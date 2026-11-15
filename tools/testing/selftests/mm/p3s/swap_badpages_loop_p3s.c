// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/loop.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

/* Only formats a new, test-owned file, never an existing block device. */
#define NR_BADPAGES 3
#define SWAP_BYTES (32 * 1024 * 1024)

static int open_temp_swapfile(char *path, size_t size)
{
	const char *dir = getenv("TMPDIR");
	int fd = -1;

	if (dir && *dir &&
	    snprintf(path, size, "%s/p3s-badpages-XXXXXX", dir) < (int)size)
		fd = mkstemp(path);
	if (fd < 0 &&
	    snprintf(path, size, "./p3s-badpages-XXXXXX") < (int)size)
		fd = mkstemp(path);
	return fd;
}

FIXTURE(badpages_loop) {
};

FIXTURE_VARIANT(badpages_loop) {
	unsigned int header_size;
	bool swapped;
};

FIXTURE_VARIANT_ADD(badpages_loop, native) {
	.header_size = NATIVE_PAGE_SIZE,
	.swapped = false,
};

FIXTURE_VARIANT_ADD(badpages_loop, compat_4k) {
	.header_size = PROCESS_PAGE_SIZE,
	.swapped = false,
};

FIXTURE_VARIANT_ADD(badpages_loop, compat_4k_swab) {
	.header_size = PROCESS_PAGE_SIZE,
	.swapped = true,
};

FIXTURE_SETUP(badpages_loop)
{
}

FIXTURE_TEARDOWN(badpages_loop)
{
}

TEST_F(badpages_loop, valid_block_swap_header)
{
	unsigned char header[NATIVE_PAGE_SIZE] = {};
	char path[PATH_MAX], dev[64];
	unsigned int i;
	int ctl = -1, loop = -1, fd = -1, index, ret, error;
	int swapoff_ret = 0, swapoff_error = 0;
	struct loop_info64 info = { .lo_flags = LO_FLAGS_AUTOCLEAR };

	if (geteuid())
		SKIP(return, "root is required for swapon(2)");
	fd = open_temp_swapfile(path, sizeof(path));
	if (fd < 0)
		SKIP(return, "cannot create temporary file: %s",
		     strerror(errno));
	unlink(path);

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		ctl = open("/dev/block/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		error = errno;
		close(fd);
		SKIP(return, "loop-control unavailable: %s", strerror(error));
	}
	if (ftruncate(fd, SWAP_BYTES)) {
		error = errno;
		close(ctl);
		close(fd);
		ASSERT_EQ(0, error);
	}
	for (i = 0; i < 3 + NR_BADPAGES; i++) {
		uint32_t value;
		size_t offset;

		if (i < 3) {
			offset = 1024 + 4 * i;
			value = i == 0 ? 1 :
				i == 1 ? SWAP_BYTES / variant->header_size - 1 :
				NR_BADPAGES;
		} else {
			offset = 1536 + 4 * (i - 3);
			value = (i - 2) *
				(getpagesize() / variant->header_size);
		}
		if (variant->swapped)
			value = __builtin_bswap32(value);
		memcpy(header + offset, &value, sizeof(value));
	}
	memcpy(header + variant->header_size - 10, "SWAPSPACE2", 10);
	if (pwrite(fd, header, sizeof(header), 0) != sizeof(header) ||
	    fsync(fd)) {
		error = errno;
		close(ctl);
		close(fd);
		ASSERT_EQ(0, error);
	}
	index = ioctl(ctl, LOOP_CTL_GET_FREE);
	if (index < 0) {
		close(ctl);
		close(fd);
		SKIP(return, "no free loop device");
	}
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
	if (loop < 0 || ioctl(loop, LOOP_SET_FD, fd)) {
		error = errno;
		if (loop >= 0)
			close(loop);
		close(ctl);
		close(fd);
		SKIP(return, "cannot attach owned loop: %s", strerror(error));
	}
	ret = ioctl(loop, LOOP_SET_STATUS64, &info);
	if (ret) {
		error = errno;
		ioctl(loop, LOOP_CLR_FD, 0);
		close(loop);
		close(ctl);
		close(fd);
		ASSERT_EQ(0, ret) TH_LOG("loop flags: %s", strerror(error));
	}
	TH_LOG("swapon owned %s header=%u badpages=%u swab=%u",
	       dev, variant->header_size, NR_BADPAGES, variant->swapped);
	ret = syscall(SYS_swapon, dev, 0);
	error = errno;
	if (!ret) {
		swapoff_ret = syscall(SYS_swapoff, dev);
		swapoff_error = errno;
	}
	ioctl(loop, LOOP_CLR_FD, 0);
	close(loop);
	close(ctl);
	close(fd);
	EXPECT_EQ(0, ret) TH_LOG("swapon ret=%d errno=%d", ret, error);
	EXPECT_EQ(0, swapoff_ret) TH_LOG("swapoff ret=%d errno=%d",
				      swapoff_ret, swapoff_error);
}

P3S_HARNESS_4KB_MAIN
