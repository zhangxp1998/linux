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

#include "../kselftest.h"
#include "pagewalk_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define RESERVE_SIZE	(64 * 1024UL)
#define DEVICE_PATH	"/dev/" PAGEWALK_PPPS_DEVICE_NAME

static int create_memfd(void)
{
	int fd = memfd_create("pagewalk-ppps", MFD_CLOEXEC);

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
	struct pagewalk_ppps_args request = {
		.offset = 4 * USER_PAGE_SIZE,
		.length = USER_PAGE_SIZE,
	};
	const size_t file_len = 5 * USER_PAGE_SIZE;
	const size_t tail_len = 3 * USER_PAGE_SIZE;
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

	for (offset = 0; offset < file_len; offset += USER_PAGE_SIZE)
		base[offset] = 0x40 + offset / USER_PAGE_SIZE;
	for (offset = 0; offset < tail_len; offset += USER_PAGE_SIZE)
		tail[offset] = 0x60 + offset / USER_PAGE_SIZE;

	request.fd = fd;
	if (ioctl(device_fd, PAGEWALK_PPPS_IOCTL_WRITE_PROTECT, &request))
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

static bool test_clean_slice(int device_fd, unsigned long *count,
			     unsigned long *bitmap_weight)
{
	struct pagewalk_ppps_args request = {
		.offset = 4 * USER_PAGE_SIZE,
		.length = USER_PAGE_SIZE,
	};
	const size_t file_offset = USER_PAGE_SIZE;
	const size_t file_len = 4 * USER_PAGE_SIZE;
	unsigned char *base;
	int fd;

	fd = create_memfd();
	if (fd < 0)
		return false;
	base = reserve_range();
	if (base == MAP_FAILED)
		goto close_fd;
	if (mmap(base, file_len, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, fd, file_offset) == MAP_FAILED)
		goto unmap;

	base[3 * USER_PAGE_SIZE] = 0x7a;
	request.fd = fd;
	if (ioctl(device_fd, PAGEWALK_PPPS_IOCTL_CLEAN, &request))
		goto unmap;
	*count = request.count;
	*bitmap_weight = request.bitmap_weight;
	munmap(base, RESERVE_SIZE);
	close(fd);
	return true;

unmap:
	munmap(base, RESERVE_SIZE);
close_fd:
	close(fd);
	return false;
}

static int run_test(void)
{
	unsigned long clean_bitmap_weight = 0;
	unsigned long write_protected = 0;
	unsigned long cleaned = 0;
	bool clean_ok;
	bool wp_ok;
	int device_fd;

	ksft_print_header();
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	device_fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (device_fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", DEVICE_PATH,
				   strerror(errno));
	ksft_test_result(true, "open the page-walk test helper\n");

	wp_ok = test_write_protect_overrun(device_fd, &write_protected);
	ksft_test_result(wp_ok, "create a file VMA followed by anonymous pages\n");
	ksft_test_result(wp_ok && write_protected == 1,
			 "write-protect only the requested file page\n");
	ksft_print_msg("write-protected PTEs=%lu (expected 1)\n",
		       write_protected);

	clean_ok = test_clean_slice(device_fd, &cleaned,
				    &clean_bitmap_weight);
	ksft_test_result(clean_ok, "create a sliced file mapping\n");
	ksft_test_result(clean_ok && cleaned == 1,
			 "clean the dirty PTE in the requested native page\n");
	ksft_test_result(clean_ok && clean_bitmap_weight == 1,
			 "record the correct native page in the dirty bitmap\n");
	ksft_print_msg("cleaned PTEs=%lu bitmap weight=%lu (expected 1, 1)\n",
		       cleaned, clean_bitmap_weight);

	close(device_fd);
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
	execl("/proc/self/exe", "pagewalk_ppps", "--run", NULL);
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
