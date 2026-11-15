// SPDX-License-Identifier: GPL-2.0
/* Verify that 4K P3S processes cannot create videobuf2 USERPTR queues. */
#define _GNU_SOURCE

#include <sys/ioctl.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define VB2_USERPTR_REJECT_P3S_RUN _IO('v', 0x71)

static bool compat_process;

TEST(videobuf2_userptr_queue_policy)
{
	int ret;
	int fd;

	fd = p3s_open_fixture("/dev/vb2_userptr_reject_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "videobuf2 USERPTR fixture unavailable");
	ASSERT_GE(fd, 0);

	errno = 0;
	ret = ioctl(fd, VB2_USERPTR_REJECT_P3S_RUN);
	if (compat_process) {
		EXPECT_EQ(-1, ret);
		EXPECT_EQ(EOPNOTSUPP, errno);
	} else {
		EXPECT_EQ(0, ret);
	}
	close(fd);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], "--run", NULL);
	if (argc == 2 && !strcmp(mode, "--run")) {
		p3s_require_compat();
		compat_process = true;
		return p3s_harness_run();
	}
	if (argc == 2 && !strcmp(mode, "--native"))
		exec_native(argv[0], "--native-run", NULL);
	if (argc == 2 && !strcmp(mode, "--native-run"))
		return p3s_harness_run();
	return EXIT_FAILURE;
}
