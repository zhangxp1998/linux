// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/zram_ioctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE	0x10000000
#endif

#define COMPAT_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE	16384UL
#define DIRECT_MAPPING_ADDR	0x7fef000000UL
#define STRIDE_MAPPING_ADDR	0x7ff0000000UL
#define ALIGN_MAPPING_ADDR	0x7ff1000000UL
#define ADDRESS_TAG		(UINT64_C(0xb4) << 56)
#define PAGEMAP_SWAPPED		UINT64_C(0x4000000000000000)
#define TEST_VALUE		0x5a

struct child_info {
	uintptr_t direct_mapping;
	uintptr_t stride_mapping;
	uintptr_t align_mapping;
	uint64_t direct_pagemap_entry;
	uint64_t stride_pagemap_entry;
	long page_size;
};

static bool write_full(int fd, const void *buffer, size_t length)
{
	const unsigned char *pos = buffer;

	while (length) {
		ssize_t written = write(fd, pos, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		pos += written;
		length -= written;
	}
	return true;
}

static bool read_full(int fd, void *buffer, size_t length)
{
	unsigned char *pos = buffer;

	while (length) {
		ssize_t bytes = read(fd, pos, length);

		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (!bytes)
			return false;
		pos += bytes;
		length -= bytes;
	}
	return true;
}

static int read_pagemap(uintptr_t address, uint64_t *entry)
{
	off_t offset = address / COMPAT_PAGE_SIZE * sizeof(*entry);
	int fd;
	int ret = -1;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (pread(fd, entry, sizeof(*entry), offset) == sizeof(*entry))
		ret = 0;
	close(fd);
	return ret;
}

static int page_out(uintptr_t address, uint64_t *entry)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (madvise((void *)address, COMPAT_PAGE_SIZE, MADV_PAGEOUT))
			return -1;
		if (read_pagemap(address, entry))
			return -1;
		if (*entry & PAGEMAP_SWAPPED)
			return 0;
		usleep(10000);
	}
	return 0;
}

static int run_child(int info_fd, int command_fd)
{
	unsigned char *align_mapping;
	unsigned char *direct_mapping;
	unsigned char *stride_mapping;
	struct child_info info = {};
	char command;

	direct_mapping = mmap((void *)DIRECT_MAPPING_ADDR, NATIVE_PAGE_SIZE,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			      -1, 0);
	stride_mapping = mmap((void *)STRIDE_MAPPING_ADDR, NATIVE_PAGE_SIZE,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			      -1, 0);
	align_mapping = mmap((void *)ALIGN_MAPPING_ADDR, NATIVE_PAGE_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			     -1, 0);
	if (direct_mapping == MAP_FAILED || stride_mapping == MAP_FAILED ||
	    align_mapping == MAP_FAILED)
		return EXIT_FAILURE;

	memset(direct_mapping, TEST_VALUE, COMPAT_PAGE_SIZE);
	memset(stride_mapping + COMPAT_PAGE_SIZE, TEST_VALUE,
	       COMPAT_PAGE_SIZE);
	if (page_out((uintptr_t)direct_mapping,
		     &info.direct_pagemap_entry) ||
	    page_out((uintptr_t)stride_mapping + COMPAT_PAGE_SIZE,
		     &info.stride_pagemap_entry))
		return EXIT_FAILURE;

	info.direct_mapping = (uintptr_t)direct_mapping;
	info.stride_mapping = (uintptr_t)stride_mapping;
	info.align_mapping = (uintptr_t)align_mapping;
	info.page_size = sysconf(_SC_PAGESIZE);
	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;

	return direct_mapping[0] == TEST_VALUE &&
	       stride_mapping[COMPAT_PAGE_SIZE] == TEST_VALUE ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_compat_child(int info_fd, int command_fd)
{
	char command_fd_arg[16];
	char info_fd_arg[16];
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
	execl("/proc/self/exe", "zram_process_range_ppps", "--child",
	      info_fd_arg, command_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_parent(const char *device)
{
	struct zram_android_ioc_process_range_writeback request = {};
	struct child_info info;
	int command_pipe[2];
	int info_pipe[2];
	int child_status;
	int direct_error;
	int stride_error;
	int align_error;
	int tagged_error;
	int direct_result;
	int stride_result;
	int align_result;
	int tagged_result;
	int pidfd;
	int zram_fd;
	char command = 1;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(13);
	ksft_test_result(sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE,
			 "caller uses native 16K pages\n");
	if (pipe(info_pipe) || pipe(command_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(info_pipe[0]);
		close(command_pipe[1]);
		_exit(exec_compat_child(info_pipe[1], command_pipe[0]));
	}
	close(info_pipe[1]);
	close(command_pipe[0]);
	if (!read_full(info_pipe[0], &info, sizeof(info)))
		ksft_exit_fail_msg("failed to read child information\n");
	close(info_pipe[0]);

	ksft_print_msg("target page size=%ld, direct=%#lx, stride=%#lx, align=%#lx\n",
		       info.page_size, (unsigned long)info.direct_mapping,
		       (unsigned long)info.stride_mapping,
		       (unsigned long)info.align_mapping);
	ksft_print_msg("pagemap entries=%#llx/%#llx\n",
		       (unsigned long long)info.direct_pagemap_entry,
		       (unsigned long long)info.stride_pagemap_entry);
	ksft_test_result(info.page_size == COMPAT_PAGE_SIZE,
			 "target uses 4K compat pages\n");
	ksft_test_result(!(info.direct_mapping & (NATIVE_PAGE_SIZE - 1)) &&
			 !(info.stride_mapping & (NATIVE_PAGE_SIZE - 1)) &&
			 !(info.align_mapping & (NATIVE_PAGE_SIZE - 1)),
			 "mixed-process test mappings are native-page aligned\n");
	ksft_test_result(info.direct_pagemap_entry & PAGEMAP_SWAPPED,
			 "direct first 4K slice is swapped out\n");
	ksft_test_result(info.stride_pagemap_entry & PAGEMAP_SWAPPED,
			 "second 4K slice is swapped out\n");

	zram_fd = open(device, O_RDWR | O_CLOEXEC);
	if (zram_fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", device,
				   strerror(errno));
	pidfd = syscall(__NR_pidfd_open, child, 0);
	if (pidfd < 0)
		ksft_exit_fail_msg("pidfd_open failed: %s\n", strerror(errno));

	request.pidfd = pidfd;
	request.start_addr = info.direct_mapping;
	request.size = NATIVE_PAGE_SIZE;
	errno = 0;
	direct_result = ioctl(zram_fd,
			      ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			      &request);
	direct_error = errno;
	ksft_test_result(!direct_result,
			 "direct range writeback succeeds (%s)\n",
			 direct_result ? strerror(direct_error) : "ok");
	ksft_test_result(!direct_result &&
			 request.written_bytes >= NATIVE_PAGE_SIZE,
			 "selected swap device contains a direct candidate (%llu bytes)\n",
			 (unsigned long long)request.written_bytes);

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = info.stride_mapping;
	request.size = NATIVE_PAGE_SIZE;
	errno = 0;
	stride_result = ioctl(zram_fd,
			      ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			      &request);
	stride_error = errno;
	ksft_test_result(!stride_result,
			 "range writeback accepts a native-aligned start (%s)\n",
			 stride_result ? strerror(stride_error) : "ok");
	ksft_test_result(!stride_result &&
			 request.written_bytes >= NATIVE_PAGE_SIZE,
			 "walker visits the swapped second 4K slice (%llu bytes)\n",
			 (unsigned long long)request.written_bytes);
	ksft_test_result(!stride_result &&
			 request.next_addr ==
			 info.stride_mapping + 2 * COMPAT_PAGE_SIZE,
			 "next address advances at the target 4K granularity (%#llx)\n",
			 (unsigned long long)request.next_addr);

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = info.align_mapping + COMPAT_PAGE_SIZE;
	errno = 0;
	align_result = ioctl(zram_fd,
			     ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			     &request);
	align_error = errno;
	ksft_test_result(!align_result,
			 "accept a target-4K-aligned range start (%s)\n",
			 align_result ? strerror(align_error) : "ok");

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = (info.align_mapping + COMPAT_PAGE_SIZE) |
			     ADDRESS_TAG;
	errno = 0;
	tagged_result = ioctl(zram_fd,
			      ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			      &request);
	tagged_error = errno;
	ksft_test_result(!tagged_result,
			 "accept a tagged target range start (%s)\n",
			 tagged_result ? strerror(tagged_error) : "ok");

	close(pidfd);
	close(zram_fd);
	write_full(command_pipe[1], &command, sizeof(command));
	close(command_pipe[1]);
	waitpid(child, &child_status, 0);
	ksft_test_result(WIFEXITED(child_status) &&
			 WEXITSTATUS(child_status) == EXIT_SUCCESS,
			 "4K target reads back the written-back page\n");
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	if (argc != 2) {
		fprintf(stderr, "usage: %s <writeback-zram-device>\n", argv[0]);
		return EXIT_FAILURE;
	}
	return run_parent(argv[1]);
}
