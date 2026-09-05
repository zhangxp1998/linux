// SPDX-License-Identifier: GPL-2.0
/*
 * A native caller runs ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK over a 4K
 * compat child's swapped-out 4K slices: native-aligned, 4K-aligned and
 * tagged range starts are accepted, the walker advances at 4K granularity,
 * and the child reads the written-back data back.
 */
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <linux/zram_ioctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define DIRECT_MAPPING_ADDR	0x7fef000000UL
#define STRIDE_MAPPING_ADDR	0x7ff0000000UL
#define ALIGN_MAPPING_ADDR	0x7ff1000000UL
#define ADDRESS_TAG		(UINT64_C(0xb4) << 56)
#define TEST_VALUE		0x5a

struct child_info {
	uintptr_t direct_mapping;
	uintptr_t stride_mapping;
	uintptr_t align_mapping;
	uint64_t direct_pagemap_entry;
	uint64_t stride_first_pagemap_entry;
	uint64_t stride_pagemap_entry;
	long page_size;
};

static int page_out(uintptr_t address, uint64_t *entry)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (madvise((void *)address, PROCESS_PAGE_SIZE, MADV_PAGEOUT))
			return -1;
		if (!ppps_pagemap_entry_of(0, PROCESS_PAGE_SIZE, address, entry))
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

	memset(direct_mapping, TEST_VALUE, PROCESS_PAGE_SIZE);
	memset(stride_mapping + PROCESS_PAGE_SIZE, TEST_VALUE,
	       PROCESS_PAGE_SIZE);
	if (page_out((uintptr_t)direct_mapping,
		     &info.direct_pagemap_entry) ||
	    page_out((uintptr_t)stride_mapping + PROCESS_PAGE_SIZE,
		     &info.stride_pagemap_entry))
		return EXIT_FAILURE;
	if (!ppps_pagemap_entry_of(0, PROCESS_PAGE_SIZE,
				   (uintptr_t)stride_mapping,
				   &info.stride_first_pagemap_entry))
		return EXIT_FAILURE;

	info.direct_mapping = (uintptr_t)direct_mapping;
	info.stride_mapping = (uintptr_t)stride_mapping;
	info.align_mapping = (uintptr_t)align_mapping;
	info.page_size = sysconf(_SC_PAGESIZE);
	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;

	return direct_mapping[0] == TEST_VALUE &&
	       stride_mapping[PROCESS_PAGE_SIZE] == TEST_VALUE ?
		EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_compat_child(int info_fd, int command_fd)
{
	char command_fd_arg[16];
	char info_fd_arg[16];

	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
	ppps_execl(true, NULL, "--child", info_fd_arg, command_fd_arg, NULL);
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
	int high_error;
	int unaligned_error;
	int direct_result;
	int stride_result;
	int align_result;
	int tagged_result;
	int high_result;
	int unaligned_result;
	int pidfd;
	int zram_fd;
	char command = 1;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(16);
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
	ksft_print_msg("pagemap entries=%#llx/%#llx/%#llx\n",
		       (unsigned long long)info.direct_pagemap_entry,
		       (unsigned long long)info.stride_first_pagemap_entry,
		       (unsigned long long)info.stride_pagemap_entry);
	ksft_test_result(info.page_size == PROCESS_PAGE_SIZE,
			 "target uses 4K compat pages\n");
	ksft_test_result(!(info.direct_mapping & (NATIVE_PAGE_SIZE - 1)) &&
			 !(info.stride_mapping & (NATIVE_PAGE_SIZE - 1)) &&
			 !(info.align_mapping & (NATIVE_PAGE_SIZE - 1)),
			 "mixed-process test mappings are native-page aligned\n");
	ksft_test_result(info.direct_pagemap_entry & PAGEMAP_SWAPPED,
			 "direct first 4K slice is swapped out\n");
	ksft_test_result(info.stride_pagemap_entry & PAGEMAP_SWAPPED,
			 "second 4K slice is swapped out\n");
	ksft_test_result(info.stride_first_pagemap_entry & PAGEMAP_SWAPPED,
			 "packed tuple also swaps the first 4K slice\n");

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
			 info.stride_mapping + PROCESS_PAGE_SIZE,
			 "next address advances at the target 4K granularity (%#llx)\n",
			 (unsigned long long)request.next_addr);

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = info.align_mapping + PROCESS_PAGE_SIZE;
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
	request.start_addr = (info.align_mapping + PROCESS_PAGE_SIZE) |
			     ADDRESS_TAG;
	errno = 0;
	tagged_result = ioctl(zram_fd,
			      ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			      &request);
	tagged_error = errno;
	ksft_test_result(!tagged_result,
			 "accept a tagged target range start (%s)\n",
			 tagged_result ? strerror(tagged_error) : "ok");

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = UINT64_MAX;
	errno = 0;
	high_result = ioctl(zram_fd,
			    ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
			    &request);
	high_error = errno;
	ksft_test_result(high_result == -1 && high_error == EINVAL,
			 "reject a range start beyond the target address space (%s)\n",
			 high_result == -1 ? strerror(high_error) : "unexpected success");

	memset(&request, 0, sizeof(request));
	request.pidfd = pidfd;
	request.start_addr = info.align_mapping + 1;
	errno = 0;
	unaligned_result = ioctl(zram_fd,
				 ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
				 &request);
	unaligned_error = errno;
	ksft_test_result(unaligned_result == -1 && unaligned_error == EINVAL,
			 "reject a target-page-unaligned range start (%s)\n",
			 unaligned_result == -1 ? strerror(unaligned_error) :
			 "unexpected success");

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
	const char *mode = ppps_run_mode(argc, argv, NULL);

	if (argc == 4 && mode && !strcmp(mode, "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	if (argc != 2) {
		fprintf(stderr, "usage: %s <writeback-zram-device>\n", argv[0]);
		return EXIT_FAILURE;
	}
	return run_parent(argv[1]);
}
