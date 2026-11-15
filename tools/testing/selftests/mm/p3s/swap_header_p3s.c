// SPDX-License-Identifier: GPL-2.0
/* Exercise 4K swap headers on a loop block device on a 16K P3S kernel. */
#define _GNU_SOURCE

#include <fcntl.h>
#include <limits.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

#define TEST_FILE_SIZE		(64UL * 1024 * 1024)
#define SWAP_VERSION_OFFSET	1024
#define SWAP_LAST_PAGE_OFFSET	1028
#define SWAP_BADPAGES_OFFSET	1032
#define SWAP_BADPAGE_LIST_OFFSET	1536
#define SWAP_MAGIC		"SWAPSPACE2"
#define SWAP_MAGIC_SIZE		10

static int open_swap_backing(char *path, size_t size)
{
	const char *env = getenv("P3S_SWAP_HEADER_FILE");
	const char *dir = getenv("TMPDIR");
	int fd = -1;

	if (env && *env &&
	    snprintf(path, size, "%s.XXXXXX", env) < (int)size)
		fd = mkstemp(path);
	if (fd < 0 && dir && *dir &&
	    snprintf(path, size, "%s/p3s-swap-hdr-XXXXXX", dir) < (int)size)
		fd = mkstemp(path);
	if (fd < 0 &&
	    snprintf(path, size, "./p3s-swap-hdr-XXXXXX") < (int)size)
		fd = mkstemp(path);
	return fd;
}

static void store_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

FIXTURE(swap_header) {
	char path[PATH_MAX];
	char dev[64];
	int file_fd;
	int loop_fd;
	bool attached;
	bool swap_active;
};

FIXTURE_SETUP(swap_header)
{
	struct loop_info64 info = { .lo_flags = LO_FLAGS_AUTOCLEAR };
	int ctl = -1, index, i, err;

	self->file_fd = -1;
	self->loop_fd = -1;
	if (geteuid())
		SKIP(return, "root is required for swapon(2)");
	ASSERT_EQ(PROCESS_PAGE_SIZE, sysconf(_SC_PAGESIZE));

	self->file_fd = open_swap_backing(self->path, sizeof(self->path));
	if (self->file_fd < 0)
		SKIP(return, "cannot create backing file: %s", strerror(errno));
	unlink(self->path);
	self->path[0] = '\0';
	ASSERT_EQ(0, ftruncate(self->file_fd, TEST_FILE_SIZE));

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		ctl = open("/dev/block/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		SKIP(return, "loop-control unavailable: %s", strerror(errno));

	index = ioctl(ctl, LOOP_CTL_GET_FREE);
	err = errno;
	close(ctl);
	if (index < 0)
		SKIP(return, "no free loop device: %s", strerror(err));

	snprintf(self->dev, sizeof(self->dev), "/dev/block/loop%d", index);
	self->loop_fd = open(self->dev, O_RDWR | O_CLOEXEC);
	/* Android ueventd creates a newly allocated loop node asynchronously. */
	for (i = 0; self->loop_fd < 0 && errno == ENOENT && i < 20; i++) {
		usleep(50000);
		self->loop_fd = open(self->dev, O_RDWR | O_CLOEXEC);
	}
	if (self->loop_fd < 0) {
		snprintf(self->dev, sizeof(self->dev), "/dev/loop%d", index);
		self->loop_fd = open(self->dev, O_RDWR | O_CLOEXEC);
	}
	if (self->loop_fd < 0)
		SKIP(return, "cannot open loop device: %s", strerror(errno));
	if (ioctl(self->loop_fd, LOOP_SET_FD, self->file_fd))
		SKIP(return, "cannot attach loop device: %s", strerror(errno));
	self->attached = true;
	ASSERT_EQ(0, ioctl(self->loop_fd, LOOP_SET_STATUS64, &info));
}

FIXTURE_TEARDOWN(swap_header)
{
	if (self->swap_active) {
		EXPECT_EQ(0, syscall(SYS_swapoff, self->dev));
		self->swap_active = false;
	}
	if (self->attached) {
		ioctl(self->loop_fd, LOOP_CLR_FD, 0);
		self->attached = false;
	}
	if (self->loop_fd >= 0) {
		close(self->loop_fd);
		self->loop_fd = -1;
	}
	if (self->file_fd >= 0) {
		close(self->file_fd);
		self->file_fd = -1;
	}
	if (self->path[0])
		unlink(self->path);
}

static bool write_swap_header(struct _test_data_swap_header *self,
			      size_t header_page_size, uint32_t last_page,
			      const uint32_t *badpages, uint32_t nr_badpages)
{
	unsigned char *header;
	uint32_t i;
	bool ok;

	header = calloc(1, NATIVE_PAGE_SIZE);
	if (!header)
		return false;
	store_u32(header, SWAP_VERSION_OFFSET, 1);
	store_u32(header, SWAP_LAST_PAGE_OFFSET, last_page);
	store_u32(header, SWAP_BADPAGES_OFFSET, nr_badpages);
	for (i = 0; i < nr_badpages; i++)
		store_u32(header, SWAP_BADPAGE_LIST_OFFSET + i * sizeof(uint32_t),
			  badpages[i]);
	memcpy(header + header_page_size - SWAP_MAGIC_SIZE,
	       SWAP_MAGIC, SWAP_MAGIC_SIZE);
	ok = pwrite(self->loop_fd, header, NATIVE_PAGE_SIZE, 0) ==
	     NATIVE_PAGE_SIZE &&
	     !fsync(self->loop_fd);
	free(header);
	return ok;
}

static bool swap_entry(const char *swap_dev, unsigned long *size_kb)
{
	char device[PATH_MAX];
	char type[32];
	char *line = NULL;
	size_t capacity = 0;
	unsigned long used_kb;
	bool found = false;
	int priority;
	FILE *file;

	file = fopen("/proc/swaps", "re");
	if (!file)
		return false;
	while (getline(&line, &capacity, file) >= 0) {
		if (sscanf(line, "%4095s %31s %lu %lu %d", device, type,
			   size_kb, &used_kb, &priority) == 5 &&
		    !strcmp(device, swap_dev)) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(file);
	return found;
}

static bool current_header_is_rejected(struct _test_data_swap_header *self)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_swapon, self->dev, 0);
	if (!ret) {
		self->swap_active = true;
		if (syscall(SYS_swapoff, self->dev))
			return false;
		self->swap_active = false;
	}
	return ret == -1 && errno == EINVAL;
}

TEST_F(swap_header, native_header_keeps_native_capacity)
{
	const uint32_t last_page = TEST_FILE_SIZE / NATIVE_PAGE_SIZE - 1;
	const unsigned long expected_kb =
		(TEST_FILE_SIZE - NATIVE_PAGE_SIZE) / 1024;
	unsigned long size_kb = 0;

	ASSERT_TRUE(write_swap_header(self, NATIVE_PAGE_SIZE, last_page,
				      NULL, 0));
	ASSERT_EQ(0, syscall(SYS_swapon, self->dev, 0));
	self->swap_active = true;
	ASSERT_TRUE(swap_entry(self->dev, &size_kb));
	EXPECT_EQ(expected_kb, size_kb);
	ASSERT_EQ(0, syscall(SYS_swapoff, self->dev));
	self->swap_active = false;
}

TEST_F(swap_header, compat_header_converts_to_native_capacity)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const unsigned long expected_kb =
		(TEST_FILE_SIZE - NATIVE_PAGE_SIZE) / 1024;
	unsigned long size_kb = 0;

	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, last_page,
				      NULL, 0));
	ASSERT_EQ(0, syscall(SYS_swapon, self->dev, 0));
	self->swap_active = true;
	ASSERT_TRUE(swap_entry(self->dev, &size_kb));
	EXPECT_EQ(expected_kb, size_kb);
	ASSERT_EQ(0, syscall(SYS_swapoff, self->dev));
	self->swap_active = false;
}

TEST_F(swap_header, compat_badpages_deduplicate_to_one_native_page)
{
	/* Both 4K bad pages fall in native 16K page 1 and must collapse to one. */
	static const uint32_t badpages[] = { 4, 5 };
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const unsigned long expected_kb =
		(TEST_FILE_SIZE - 2 * NATIVE_PAGE_SIZE) / 1024;
	unsigned long size_kb = 0;

	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, last_page,
				      badpages, ARRAY_SIZE(badpages)));
	ASSERT_EQ(0, syscall(SYS_swapon, self->dev, 0));
	self->swap_active = true;
	ASSERT_TRUE(swap_entry(self->dev, &size_kb));
	EXPECT_EQ(expected_kb, size_kb);
	ASSERT_EQ(0, syscall(SYS_swapoff, self->dev));
	self->swap_active = false;
}

TEST_F(swap_header, sub_native_area_rejected)
{
	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, 2, NULL, 0));
	EXPECT_TRUE(current_header_is_rejected(self));
}

TEST_F(swap_header, badpage_overlapping_header_rejected)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const uint32_t badpage_zero = 0;
	const uint32_t badpage_slice = 1;

	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, last_page,
				      &badpage_zero, 1));
	EXPECT_TRUE(current_header_is_rejected(self));
	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, last_page,
				      &badpage_slice, 1));
	EXPECT_TRUE(current_header_is_rejected(self));
}

TEST_F(swap_header, out_of_range_badpage_rejected)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const uint32_t badpage = UINT32_MAX;

	ASSERT_TRUE(write_swap_header(self, PROCESS_PAGE_SIZE, last_page,
				      &badpage, 1));
	EXPECT_TRUE(current_header_is_rejected(self));
}

P3S_HARNESS_4KB_MAIN
