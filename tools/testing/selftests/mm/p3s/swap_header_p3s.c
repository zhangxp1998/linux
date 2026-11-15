// SPDX-License-Identifier: GPL-2.0
/* Exercise 4K swap headers on a native 16K P3S kernel. */
#define _GNU_SOURCE

#include <fcntl.h>
#include <limits.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

#define TEST_FILE_SIZE	(64UL * 1024 * 1024)
#define SWAP_VERSION_OFFSET	1024
#define SWAP_LAST_PAGE_OFFSET	1028
#define SWAP_BADPAGES_OFFSET	1032
#define SWAP_BADPAGE_LIST_OFFSET	1536
#define SWAP_MAGIC	"SWAPSPACE2"
#define SWAP_MAGIC_SIZE	10

static const char *swap_path;
static bool swap_active;

static void store_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static bool cleanup(void)
{
	bool ok = true;

	if (swap_active)
		ok = !syscall(SYS_swapoff, swap_path);
	if (swap_path)
		ok &= !unlink(swap_path);
	swap_active = false;
	swap_path = NULL;
	return ok;
}

static bool write_swap_header(size_t header_page_size, uint32_t last_page,
			      const uint32_t *badpages, uint32_t nr_badpages)
{
	unsigned char *header;
	uint32_t i;
	int error;
	int fd;
	bool ok = false;

	header = calloc(1, NATIVE_PAGE_SIZE);
	if (!header)
		return false;
	fd = open(swap_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		goto out_free;
	error = posix_fallocate(fd, 0, TEST_FILE_SIZE);
	if (error) {
		errno = error;
		goto out_close;
	}
	store_u32(header, SWAP_VERSION_OFFSET, 1);
	store_u32(header, SWAP_LAST_PAGE_OFFSET, last_page);
	store_u32(header, SWAP_BADPAGES_OFFSET, nr_badpages);
	for (i = 0; i < nr_badpages; i++)
		store_u32(header, SWAP_BADPAGE_LIST_OFFSET + i * sizeof(uint32_t),
			  badpages[i]);
	memcpy(header + header_page_size - SWAP_MAGIC_SIZE,
	       SWAP_MAGIC, SWAP_MAGIC_SIZE);
	if (pwrite(fd, header, header_page_size, 0) !=
	    (ssize_t)header_page_size ||
	    fsync(fd))
		goto out_close;
	ok = !close(fd);
	fd = -1;
out_close:
	if (fd >= 0)
		close(fd);
out_free:
	free(header);
	return ok;
}

static bool swap_entry(unsigned long *size_kb)
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
		    !strcmp(device, swap_path)) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(file);
	return found;
}

static bool native_page_capacity(unsigned long size_kb)
{
	return size_kb && size_kb < TEST_FILE_SIZE / 1024 &&
	       !(size_kb % (NATIVE_PAGE_SIZE / 1024));
}

static bool current_header_is_rejected(void)
{
	int ret;

	errno = 0;
	ret = syscall(SYS_swapon, swap_path, 0);
	if (!ret) {
		swap_active = true;
		if (syscall(SYS_swapoff, swap_path))
			return false;
		swap_active = false;
	}
	return ret == -1 && errno == EINVAL;
}

FIXTURE(swap_header)
{
	char path[PATH_MAX];
};

FIXTURE_SETUP(swap_header)
{
	const char *base = getenv("P3S_SWAP_HEADER_FILE");
	int fd, length;

	if (!base)
		SKIP(return, "P3S_SWAP_HEADER_FILE is not set");
	if (geteuid())
		SKIP(return, "root is required for swapon(2)");
	ASSERT_EQ(PROCESS_PAGE_SIZE, sysconf(_SC_PAGESIZE));
	length = snprintf(self->path, sizeof(self->path), "%s.XXXXXX", base);
	ASSERT_GT(length, 0);
	ASSERT_LT(length, (int)sizeof(self->path));
	fd = mkstemp(self->path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, close(fd));
	swap_path = self->path;
}

FIXTURE_TEARDOWN(swap_header)
{
	if (swap_path)
		EXPECT_TRUE(cleanup());
}

TEST_F(swap_header, native_header_keeps_native_capacity)
{
	const uint32_t last_page = TEST_FILE_SIZE / NATIVE_PAGE_SIZE - 1;
	unsigned long size_kb = 0;

	ASSERT_TRUE(write_swap_header(NATIVE_PAGE_SIZE, last_page, NULL, 0));
	ASSERT_EQ(0, syscall(SYS_swapon, swap_path, 0));
	swap_active = true;
	ASSERT_TRUE(swap_entry(&size_kb));
	EXPECT_TRUE(native_page_capacity(size_kb));
	ASSERT_EQ(0, syscall(SYS_swapoff, swap_path));
	swap_active = false;
}

TEST_F(swap_header, compat_header_converts_to_native_capacity)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	unsigned long size_kb = 0;

	ASSERT_TRUE(write_swap_header(PROCESS_PAGE_SIZE, last_page, NULL, 0));
	ASSERT_EQ(0, syscall(SYS_swapon, swap_path, 0));
	swap_active = true;
	ASSERT_TRUE(swap_entry(&size_kb));
	EXPECT_TRUE(native_page_capacity(size_kb));
	ASSERT_EQ(0, syscall(SYS_swapoff, swap_path));
	swap_active = false;
}

TEST_F(swap_header, duplicate_and_invalid_badpages_rejected)
{
	static const uint32_t badpages[] = { 0, 4, 5, 8, 9, UINT32_MAX };
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	int ret, error;

	ASSERT_TRUE(write_swap_header(PROCESS_PAGE_SIZE, last_page, badpages,
				      ARRAY_SIZE(badpages)));
	errno = 0;
	ret = syscall(SYS_swapon, swap_path, 0);
	error = errno;
	if (!ret)
		swap_active = true;
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, error);
}

TEST_F(swap_header, sub_native_area_rejected)
{
	int ret, error;

	ASSERT_TRUE(write_swap_header(PROCESS_PAGE_SIZE, 2, NULL, 0));
	errno = 0;
	ret = syscall(SYS_swapon, swap_path, 0);
	error = errno;
	if (!ret)
		swap_active = true;
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, error);
}

TEST_F(swap_header, badpage_zero_rejected)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const uint32_t badpage = 0;

	ASSERT_TRUE(write_swap_header(PROCESS_PAGE_SIZE, last_page,
				      &badpage, 1));
	EXPECT_TRUE(current_header_is_rejected());
}

TEST_F(swap_header, out_of_range_badpage_rejected)
{
	const uint32_t last_page = TEST_FILE_SIZE / PROCESS_PAGE_SIZE - 1;
	const uint32_t badpage = UINT32_MAX;

	ASSERT_TRUE(write_swap_header(PROCESS_PAGE_SIZE, last_page,
				      &badpage, 1));
	EXPECT_TRUE(current_header_is_rejected());
}

P3S_HARNESS_COMPAT_MAIN
