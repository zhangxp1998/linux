// SPDX-License-Identifier: GPL-2.0
/*
 * The in-kernel page walker fixture write-protects only the requested 4K
 * file slice of a compat process, without touching adjacent anonymous pages.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "pagewalk_p3s.h"

#define RESERVE_SIZE	(64 * 1024UL)
#define DEVICE_PATH	"/dev/" PAGEWALK_P3S_DEVICE_NAME

static int create_memfd(void)
{
	int fd = memfd_create("pagewalk-p3s", MFD_CLOEXEC);

	if (fd >= 0 && ftruncate(fd, RESERVE_SIZE)) {
		close(fd);
		return -1;
	}
	return fd;
}

static unsigned char *reserve_range(void)
{
	return mmap(NULL, RESERVE_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static bool test_write_protect_overrun(int device_fd, unsigned long *count)
{
	struct pagewalk_p3s_args request = {
		.offset = 4 * PROCESS_PAGE_SIZE,
		.length = PROCESS_PAGE_SIZE,
	};
	const size_t file_len = 5 * PROCESS_PAGE_SIZE;
	const size_t tail_len = 3 * PROCESS_PAGE_SIZE;
	unsigned char *base;
	unsigned char *tail;
	size_t offset;
	int fd;

	fd = create_memfd();
	if (fd < 0)
		return false;
	base = reserve_range();
	if (base == MAP_FAILED)
		goto close_fd;
	if (mmap(base, file_len, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
		goto unmap;
	tail = mmap(base + file_len, tail_len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (tail == MAP_FAILED)
		goto unmap;

	for (offset = 0; offset < file_len; offset += PROCESS_PAGE_SIZE)
		base[offset] = 0x40 + offset / PROCESS_PAGE_SIZE;
	for (offset = 0; offset < tail_len; offset += PROCESS_PAGE_SIZE)
		tail[offset] = 0x60 + offset / PROCESS_PAGE_SIZE;

	request.fd = fd;
	if (ioctl(device_fd, PAGEWALK_P3S_IOCTL_WRITE_PROTECT, &request))
		goto unmap;
	*count = request.count;
	munmap(base, RESERVE_SIZE);
	close(fd);
	return true;

unmap:
	munmap(base, RESERVE_SIZE);
close_fd:
	close(fd);
	return false;
}

TEST(p3s_pagewalk)
{
	unsigned long write_protected = 0;
	bool wp_ok;
	int device_fd;

	device_fd = p3s_open_fixture_or_skip(DEVICE_PATH, O_RDWR);
	EXPECT_TRUE(true);

	wp_ok = test_write_protect_overrun(device_fd, &write_protected);
	EXPECT_TRUE(wp_ok);
	EXPECT_TRUE(wp_ok && write_protected == 1);
	ksft_print_msg("write-protected PTEs=%lu (expected 1)\n",
		       write_protected);

	close(device_fd);
}

P3S_HARNESS_COMPAT_MAIN
