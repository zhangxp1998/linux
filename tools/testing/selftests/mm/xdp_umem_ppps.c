// SPDX-License-Identifier: GPL-2.0
/*
 * AF_XDP UMEM registration by a 4K compat process accepts a one-4K-page UMEM
 * both with an untagged address and with a top-byte-tagged one.
 */
#define _GNU_SOURCE

#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "kselftest_ppps.h"

#define TAG_MASK		(0xb4ULL << 56)

static int register_umem(uint64_t address)
{
	struct xdp_umem_reg registration = {
		.addr = address,
		.len = PROCESS_PAGE_SIZE,
		.chunk_size = 2048,
	};
	int fd;
	int ret;

	fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	ret = setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &registration,
			 sizeof(registration));
	if (ret)
		ret = -errno;
	close(fd);
	return ret;
}

static int run_test(void)
{
	unsigned char *mapping;
	uint64_t tagged;
	int untagged_result;
	int tagged_result;

	ksft_print_header();
	ksft_set_plan(4);
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(mapping, 0x5a, PROCESS_PAGE_SIZE);
	ksft_test_result(!((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1)),
			 "UMEM starts at native-page slice zero\n");

	untagged_result = register_umem((uintptr_t)mapping);
	if (untagged_result == -EAFNOSUPPORT ||
	    untagged_result == -EPROTONOSUPPORT)
		ksft_exit_skip("AF_XDP unavailable: %s\n",
			       strerror(-untagged_result));
	ksft_test_result(!untagged_result,
			 "register an untagged one-page AF_XDP UMEM (%s)\n",
			 untagged_result ? strerror(-untagged_result) : "ok");

	tagged = (uintptr_t)mapping | TAG_MASK;
	ksft_test_result((tagged & (PROCESS_PAGE_SIZE - 1)) == 0 &&
			 (tagged & TAG_MASK) == TAG_MASK,
			 "construct an aligned top-byte-tagged UMEM address\n");
	tagged_result = register_umem(tagged);
	ksft_test_result(!tagged_result,
			 "register the same tagged AF_XDP UMEM (%s)\n",
			 tagged_result ? strerror(-tagged_result) : "ok");
	munmap(mapping, NATIVE_PAGE_SIZE);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
