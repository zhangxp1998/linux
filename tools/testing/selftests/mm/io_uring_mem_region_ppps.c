// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/stddef.h>

#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H
struct __kernel_timespec {
	long long tv_sec;
	long long tv_nsec;
};

#include <linux/io_uring.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define PROCESS_PAGE_SIZE 4096UL
#define NATIVE_16K_PAGE_SIZE 16384UL
#define IORING_MAP_OFF_PARAM_REGION 0x20000000ULL

static int setup_ring(unsigned int entries, struct io_uring_params *params)
{
	return syscall(__NR_io_uring_setup, entries, params);
}

static int register_ring(int fd, unsigned int opcode, void *arg,
			 unsigned int nr_args)
{
	return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

static int enter_ring(int fd, unsigned int flags, const void *arg,
		      size_t argsz)
{
	return syscall(__NR_io_uring_enter, fd, 0, 0, flags, arg, argsz);
}

static bool register_region(int fd, struct io_uring_region_desc *desc,
			    unsigned long flags)
{
	struct io_uring_mem_region_reg reg = {
		.region_uptr = (uintptr_t)desc,
		.flags = flags,
	};

	return register_ring(fd, IORING_REGISTER_MEM_REGION, &reg, 1) == 0;
}

static void *map_non_native_aligned_slice(void **reservation_out,
					 size_t *reservation_size_out)
{
	size_t reservation_size = 4 * NATIVE_16K_PAGE_SIZE;
	uintptr_t aligned;
	void *reservation;
	void *slice;

	reservation = mmap(NULL, reservation_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)reservation + NATIVE_16K_PAGE_SIZE - 1) &
		  ~(NATIVE_16K_PAGE_SIZE - 1);
	slice = mmap((void *)(aligned + PROCESS_PAGE_SIZE), PROCESS_PAGE_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (slice == MAP_FAILED) {
		munmap(reservation, reservation_size);
		return MAP_FAILED;
	}
	*reservation_out = reservation;
	*reservation_size_out = reservation_size;
	return slice;
}

static void *map_native_aligned_region(void **reservation_out,
				       size_t *reservation_size_out)
{
	size_t reservation_size = 4 * NATIVE_16K_PAGE_SIZE;
	uintptr_t aligned;
	void *reservation;
	void *region;

	reservation = mmap(NULL, reservation_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)reservation + NATIVE_16K_PAGE_SIZE - 1) &
		  ~(NATIVE_16K_PAGE_SIZE - 1);
	region = mmap((void *)aligned, NATIVE_16K_PAGE_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (region == MAP_FAILED) {
		munmap(reservation, reservation_size);
		return MAP_FAILED;
	}
	*reservation_out = reservation;
	*reservation_size_out = reservation_size;
	return region;
}

static int run_test(void)
{
	struct io_uring_region_desc kernel_desc = {
		.size = PROCESS_PAGE_SIZE,
	};
	struct io_uring_region_desc user_desc = {
		.size = PROCESS_PAGE_SIZE,
		.flags = IORING_MEM_REGION_TYPE_USER,
	};
	struct io_uring_params kernel_params = {};
	struct io_uring_params user_params = {
		.flags = IORING_SETUP_R_DISABLED,
	};
	struct io_uring_region_desc multi_desc = {
		.size = NATIVE_16K_PAGE_SIZE,
		.flags = IORING_MEM_REGION_TYPE_USER,
	};
	struct io_uring_params multi_params = {
		.flags = IORING_SETUP_R_DISABLED,
	};
	struct io_uring_reg_wait *wait_arg = MAP_FAILED;
	struct io_uring_reg_wait *multi_wait_arg;
	void *reservation = MAP_FAILED;
	void *multi_reservation = MAP_FAILED;
	void *multi_region = MAP_FAILED;
	size_t reservation_size = 0;
	size_t multi_reservation_size = 0;
	unsigned char *kernel_mapping = MAP_FAILED;
	bool kernel_registered = false;
	bool kernel_contents_ok = false;
	bool user_registered = false;
	bool rings_enabled = false;
	bool multi_registered = false;
	bool multi_safely_rejected = false;
	bool multi_rings_enabled = false;
	int enter_ret = -1;
	int multi_enter_ret = -1;
	int kernel_fd;
	int user_fd;
	int multi_fd;
	int multi_register_errno = 0;

	ksft_print_header();
	ksft_set_plan(13);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE,
			 "process uses 4K pages\n");

	kernel_fd = setup_ring(2, &kernel_params);
	ksft_test_result(kernel_fd >= 0, "create the kernel-backed ring\n");
	if (kernel_fd >= 0)
		kernel_registered = register_region(kernel_fd, &kernel_desc, 0);
	ksft_test_result(kernel_registered,
			 "register a kernel-allocated 4K parameter region\n");
	ksft_test_result(kernel_registered &&
			 kernel_desc.mmap_offset == IORING_MAP_OFF_PARAM_REGION,
			 "return the parameter-region mmap offset\n");
	if (kernel_registered)
		kernel_mapping = mmap(NULL, PROCESS_PAGE_SIZE,
				      PROT_READ | PROT_WRITE, MAP_SHARED,
				      kernel_fd, kernel_desc.mmap_offset);
	ksft_test_result(kernel_mapping != MAP_FAILED,
			 "map exactly one 4K parameter region\n");
	if (kernel_mapping != MAP_FAILED) {
		kernel_mapping[0] = 0x5a;
		kernel_mapping[PROCESS_PAGE_SIZE - 1] = 0xa5;
		kernel_contents_ok = kernel_mapping[0] == 0x5a &&
			kernel_mapping[PROCESS_PAGE_SIZE - 1] == 0xa5;
	}
	ksft_test_result(kernel_contents_ok,
			 "read and write both ends of the 4K region\n");

	user_fd = setup_ring(2, &user_params);
	wait_arg = map_non_native_aligned_slice(&reservation,
						&reservation_size);
	ksft_test_result(user_fd >= 0 && wait_arg != MAP_FAILED,
			 "create a disabled ring and a 4K-only-aligned slice\n");
	if (wait_arg != MAP_FAILED) {
		memset(wait_arg, 0, sizeof(*wait_arg));
		wait_arg->flags = 1U << 31;
		user_desc.user_addr = (uintptr_t)wait_arg;
	}
	if (user_fd >= 0 && wait_arg != MAP_FAILED)
		user_registered = register_region(
			user_fd, &user_desc, IORING_MEM_REGION_REG_WAIT_ARG);
	ksft_test_result(user_registered,
			 "register the non-native-aligned user region\n");
	if (user_registered)
		rings_enabled = register_ring(user_fd, IORING_REGISTER_ENABLE_RINGS,
					     NULL, 0) == 0;
	ksft_test_result(rings_enabled, "enable the ring after registration\n");
	if (rings_enabled) {
		errno = 0;
		enter_ret = enter_ring(user_fd,
				       IORING_ENTER_GETEVENTS |
				       IORING_ENTER_EXT_ARG |
				       IORING_ENTER_EXT_ARG_REG,
				       NULL, sizeof(*wait_arg));
	}
	ksft_test_result(rings_enabled && enter_ret == -1 && errno == EINVAL,
			 "registered wait reads from the selected 4K slice "
			 "(ret=%d errno=%d)\n", enter_ret, errno);

	multi_fd = setup_ring(2, &multi_params);
	multi_region = map_native_aligned_region(&multi_reservation,
						 &multi_reservation_size);
	ksft_test_result(multi_fd >= 0 && multi_region != MAP_FAILED,
			 "create a multi-page user region\n");
	if (multi_region != MAP_FAILED) {
		multi_wait_arg = multi_region + PROCESS_PAGE_SIZE;
		memset(multi_wait_arg, 0, sizeof(*multi_wait_arg));
		multi_wait_arg->flags = 1U << 31;
		multi_desc.user_addr = (uintptr_t)multi_region;
	}
	if (multi_fd >= 0 && multi_region != MAP_FAILED) {
		errno = 0;
		multi_registered = register_region(
			multi_fd, &multi_desc, IORING_MEM_REGION_REG_WAIT_ARG);
		multi_register_errno = errno;
		multi_safely_rejected = !multi_registered &&
			multi_register_errno == EOPNOTSUPP;
	}
	ksft_test_result(multi_registered || multi_safely_rejected,
			 "accept or safely reject a non-contiguous PPPS region "
			 "(registered=%d errno=%d)\n", multi_registered,
			 multi_register_errno);
	if (multi_registered) {
		multi_rings_enabled = register_ring(
			multi_fd, IORING_REGISTER_ENABLE_RINGS, NULL, 0) == 0;
		if (multi_rings_enabled) {
			errno = 0;
			multi_enter_ret = enter_ring(
				multi_fd, IORING_ENTER_GETEVENTS |
				IORING_ENTER_EXT_ARG |
				IORING_ENTER_EXT_ARG_REG,
				(void *)PROCESS_PAGE_SIZE,
				sizeof(*multi_wait_arg));
		}
	}
	ksft_test_result(multi_safely_rejected ||
			 (multi_rings_enabled && multi_enter_ret == -1 &&
			  errno == EINVAL),
			 "avoid exposing gaps between PPPS backing pages "
			 "(ret=%d errno=%d)\n", multi_enter_ret, errno);

	if (kernel_mapping != MAP_FAILED)
		munmap(kernel_mapping, PROCESS_PAGE_SIZE);
	if (kernel_fd >= 0)
		close(kernel_fd);
	if (user_fd >= 0)
		close(user_fd);
	if (reservation != MAP_FAILED)
		munmap(reservation, reservation_size);
	if (multi_fd >= 0)
		close(multi_fd);
	if (multi_reservation != MAP_FAILED)
		munmap(multi_reservation, multi_reservation_size);
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
	execl("/proc/self/exe", "io_uring_mem_region_ppps", "--run", NULL);
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
