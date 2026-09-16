// SPDX-License-Identifier: GPL-2.0
/*
 * AF_XDP for a 4K compat process: a 4K-aligned, non-16K-aligned anonymous
 * UMEM page is rejected (packed compat anonymous memory cannot back a UMEM),
 * a subpage RX ring offset cookie is rejected, and every 4K slice of a
 * vmalloc'd multi-native-page RX ring maps at misaligned and aligned
 * addresses.  A native probe first decides the expected multi-page UMEM result.
 */
#define _GNU_SOURCE

#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "kselftest_ppps.h"


#define RING_ENTRIES 1024U
#define MISALIGNED_HINT ((void *)0x300001000ULL)
#define ALIGNED_HINT ((void *)0x400000000ULL)

static bool mapping_is_resident(void *mapping, size_t size)
{
	unsigned char residency[8] = {};
	size_t pages = (size + PROCESS_PAGE_SIZE - 1) / PROCESS_PAGE_SIZE;
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
	return mapping + PROCESS_PAGE_SIZE;
}

static int run_test(bool native_16k)
{
	struct xdp_umem_reg umem_reg = {
		.len = PROCESS_PAGE_SIZE,
		.chunk_size = 2048,
	};
	struct xdp_umem_reg multi_reg = {
		.len = 2 * PROCESS_PAGE_SIZE,
		.chunk_size = 2048,
	};
	struct xdp_mmap_offsets offsets;
	socklen_t offsets_len = sizeof(offsets);
	unsigned int entries = RING_ENTRIES;
	void *reservation = MAP_FAILED;
	void *invalid;
	void *misaligned;
	void *aligned;
	void *umem;
	size_t map_size;
	int saved_errno;
	int reg_ret;
	int multi_fd;
	int fd;

	ppps_require_compat();
	ksft_print_header();
	ksft_set_plan(9);
	fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	ksft_test_result(fd >= 0, "create an AF_XDP socket\n");
	if (fd < 0)
		ksft_exit_fail_msg("AF_XDP socket failed: %s\n",
				   strerror(errno));
	umem = map_compat_umem(&reservation);
	ksft_test_result(umem != MAP_FAILED &&
			 !((uintptr_t)umem & (PROCESS_PAGE_SIZE - 1)) &&
			 ((uintptr_t)umem & (NATIVE_PAGE_SIZE - 1)),
			 "map a 4K-aligned, non-16K-aligned UMEM page\n");
	umem_reg.addr = (uintptr_t)umem;
	errno = 0;
	reg_ret = umem == MAP_FAILED ? -1 :
		setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &umem_reg,
			   sizeof(umem_reg));
	saved_errno = errno;
	ksft_test_result(reg_ret == -1 && saved_errno == EOPNOTSUPP,
			 "reject a packed anonymous UMEM slice (errno=%d)\n",
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
	errno = 0;
	invalid = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, PROCESS_PAGE_SIZE);
	saved_errno = errno;
	ksft_test_result(invalid == MAP_FAILED && saved_errno == EINVAL,
			 "reject a subpage RX ring offset cookie (errno=%d)\n",
			 saved_errno);
	if (invalid != MAP_FAILED)
		munmap(invalid, PROCESS_PAGE_SIZE);

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

int main(int argc, char **argv)
{
	const char *mode = ppps_run_mode(argc, argv, NULL);

	if (!mode)
		exec_native(argv[0], "--native-probe", NULL);
	if (argc == 2 && !strcmp(mode, "--native-probe"))
		exec_compat(argv[0], sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE ?
			    "--run-16k" : "--run-4k", NULL);
	if (argc == 2 && !strcmp(mode, "--run-4k"))
		return run_test(false);
	if (argc == 2 && !strcmp(mode, "--run-16k"))
		return run_test(true);
	return EXIT_FAILURE;
}
