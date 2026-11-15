// SPDX-License-Identifier: GPL-2.0
/* 4KB processes refuse UMEM registration; native mappings remain supported. */
#define _GNU_SOURCE

#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kselftest_p3s.h"
#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"

#ifndef AF_XDP
#define AF_XDP 44
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define TAG_MASK		(0xb4ULL << 56)

static int register_umem(uint64_t address, size_t len)
{
	struct xdp_umem_reg registration = {
		.addr = address,
		.len = len,
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

TEST(p3s_xdp_umem)
{
	unsigned char *mapping;
	unsigned int tag, large;
	bool four_kb = p3s_is_4kb_process();
	int fd;

	fd = memfd_create("xdp-umem-p3s", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	if (ftruncate(fd, NATIVE_PAGE_SIZE)) {
		close(fd);
		ASSERT_TRUE(false);
	}
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED ||
	    ((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1))) {
		if (mapping != MAP_FAILED)
			munmap(mapping, NATIVE_PAGE_SIZE);
		close(fd);
		ASSERT_TRUE(false);
	}
	memset(mapping, 0x5a, NATIVE_PAGE_SIZE);
	for (large = 0; large < 2; large++) {
		size_t len = large ? NATIVE_PAGE_SIZE : PROCESS_PAGE_SIZE;

		for (tag = 0; tag < 2; tag++) {
			uint64_t address = (uintptr_t)mapping |
					   (tag ? TAG_MASK : 0);
			int ret = register_umem(address, len);

			if (ret == -EAFNOSUPPORT || ret == -EPROTONOSUPPORT ||
			    ret == -EPERM || ret == -EACCES ||
			    ret == -ENOBUFS)
				SKIP(goto out, "AF_XDP unavailable: %s",
				     strerror(-ret));
			EXPECT_TRUE(ret == (four_kb ? -EOPNOTSUPP : 0));
		}
	}
out:
	munmap(mapping, NATIVE_PAGE_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
