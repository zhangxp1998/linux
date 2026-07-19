// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/if_xdp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define RING_ENTRIES 1024U
#define MISALIGNED_HINT ((void *)0x300001000ULL)
#define ALIGNED_HINT ((void *)0x400000000ULL)

static bool mapping_is_resident(void *mapping, size_t size)
{
	unsigned char residency[8] = {};
	size_t pages = (size + USER_PAGE_SIZE - 1) / USER_PAGE_SIZE;
	size_t i;

	if (mapping == MAP_FAILED || pages > sizeof(residency) ||
	    mincore(mapping, size, residency))
		return false;
	for (i = 0; i < pages; i++) {
		if (!(residency[i] & 1))
			return false;
	}
	return true;
}

static void *map_ring(int fd, void *hint, size_t size)
{
	return mmap(hint, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_POPULATE | MAP_FIXED_NOREPLACE, fd, 0);
}

static void *map_compat_umem(void **reservation_out)
{
	uintptr_t aligned;
	void *reservation;
	void *mapping;

	reservation = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)aligned, NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, 4 * NATIVE_PAGE_SIZE);
		return MAP_FAILED;
	}
	*reservation_out = reservation;
	return mapping + USER_PAGE_SIZE;
}

static int run_test(bool native_16k)
{
	struct xdp_umem_reg umem_reg = {
		.len = USER_PAGE_SIZE,
		.chunk_size = 2048,
	};
	struct xdp_umem_reg multi_reg = {
		.len = 2 * USER_PAGE_SIZE,
		.chunk_size = 2048,
	};
	struct xdp_mmap_offsets offsets;
	socklen_t offsets_len = sizeof(offsets);
	unsigned int entries = RING_ENTRIES;
	void *reservation = MAP_FAILED;
	void *misaligned;
	void *aligned;
	void *umem;
	size_t map_size;
	int saved_errno;
	int reg_ret;
	int multi_fd;
	int fd;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	ksft_test_result(fd >= 0, "create an AF_XDP socket\n");
	if (fd < 0)
		ksft_exit_fail_msg("AF_XDP socket failed: %s\n",
				   strerror(errno));
	umem = map_compat_umem(&reservation);
	ksft_test_result(umem != MAP_FAILED &&
			 !((uintptr_t)umem & (USER_PAGE_SIZE - 1)) &&
			 ((uintptr_t)umem & (NATIVE_PAGE_SIZE - 1)),
			 "map a 4K-aligned, non-16K-aligned UMEM page\n");
	umem_reg.addr = (uintptr_t)umem;
	errno = 0;
	reg_ret = umem == MAP_FAILED ? -1 :
		setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &umem_reg,
			   sizeof(umem_reg));
	saved_errno = errno;
	ksft_test_result(reg_ret == 0,
			 "register one logical UMEM page (errno=%d)\n",
			 saved_errno);
	multi_fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (multi_fd < 0)
		ksft_exit_fail_msg("second AF_XDP socket failed: %s\n",
				   strerror(errno));
	multi_reg.addr = (uintptr_t)umem;
	errno = 0;
	reg_ret = setsockopt(multi_fd, SOL_XDP, XDP_UMEM_REG, &multi_reg,
			     sizeof(multi_reg));
	saved_errno = errno;
	ksft_test_result(native_16k ?
			 reg_ret == -1 && saved_errno == EOPNOTSUPP : reg_ret == 0,
			 "handle a multi-page UMEM safely (errno=%d)\n",
			 saved_errno);
	close(multi_fd);
	if (setsockopt(fd, SOL_XDP, XDP_RX_RING, &entries, sizeof(entries)))
		ksft_exit_fail_msg("RX ring setup failed: %s\n", strerror(errno));
	ksft_test_result_pass("configure a multi-page RX ring\n");
	if (getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS,
		       &offsets, &offsets_len))
		ksft_exit_fail_msg("read ring offsets failed: %s\n",
				   strerror(errno));
	map_size = offsets.rx.desc + entries * sizeof(struct xdp_desc);
	ksft_test_result(map_size > NATIVE_PAGE_SIZE,
			 "RX ring spans more than one native page\n");

	misaligned = map_ring(fd, MISALIGNED_HINT, map_size);
	ksft_test_result(misaligned == MISALIGNED_HINT &&
			 mapping_is_resident(misaligned, map_size),
			 "map every slice at a native-page-misaligned address\n");
	aligned = map_ring(fd, ALIGNED_HINT, map_size);
	ksft_test_result(aligned == ALIGNED_HINT &&
			 mapping_is_resident(aligned, map_size),
			 "map every slice across multiple native pages\n");

	if (misaligned != MAP_FAILED)
		munmap(misaligned, map_size);
	if (aligned != MAP_FAILED)
		munmap(aligned, map_size);
	close(fd);
	if (reservation != MAP_FAILED)
		munmap(reservation, 4 * NATIVE_PAGE_SIZE);
	ksft_finished();
}

static int exec_compat(bool native_16k)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "xsk_vmalloc_mmap_ppps",
	      native_16k ? "--run-16k" : "--run-4k", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

static int exec_native_probe(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality clear failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "xsk_vmalloc_mmap_ppps", "--native-probe",
	      NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_native_probe();
	if (argc == 2 && !strcmp(argv[1], "--native-probe"))
		return exec_compat(sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE);
	if (argc == 2 && !strcmp(argv[1], "--run-4k"))
		return run_test(false);
	if (argc == 2 && !strcmp(argv[1], "--run-16k"))
		return run_test(true);
	return EXIT_FAILURE;
}
