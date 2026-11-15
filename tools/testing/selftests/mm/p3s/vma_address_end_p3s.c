// SPDX-License-Identifier: GPL-2.0
/* vma_address_end() clips a kernel page at the VMA's 4KB file offset. */
#define _GNU_SOURCE
#include <sys/ioctl.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "vma_address_end_p3s.h"

TEST(sliced_vma_clips_rmap_end)
{
	int fd = p3s_open_fixture("/dev/vma_address_end_p3s", O_RDONLY);

	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "vma_address_end fixture unavailable");
	ASSERT_GE(fd, 0);
	EXPECT_EQ(0, ioctl(fd, VMA_ADDRESS_END_P3S_CHECK));
	close(fd);
}

P3S_HARNESS_4KB_MAIN
