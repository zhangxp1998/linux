// SPDX-License-Identifier: GPL-2.0
/* Verify that 4KB processes cannot create videobuf2 USERPTR queues. */
#define _GNU_SOURCE

#include <sys/ioctl.h>

#include "p3s_harness.h"

#define VB2_USERPTR_REJECT_P3S_REQBUFS _IO('v', 0x71)
#define VB2_USERPTR_REJECT_P3S_CREATE_BUFS _IO('v', 0x72)

TEST(videobuf2_userptr_queue_policy)
{
	int ret;
	int fd;

	fd = p3s_open_fixture("/dev/vb2_userptr_reject_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "videobuf2 USERPTR fixture unavailable");
	ASSERT_GE(fd, 0);

	errno = 0;
	ret = ioctl(fd, VB2_USERPTR_REJECT_P3S_REQBUFS);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOPNOTSUPP, errno);

	errno = 0;
	ret = ioctl(fd, VB2_USERPTR_REJECT_P3S_CREATE_BUFS);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOPNOTSUPP, errno);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
