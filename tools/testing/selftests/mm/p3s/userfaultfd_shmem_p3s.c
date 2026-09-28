// SPDX-License-Identifier: GPL-2.0
/* Verify that 4K compat userfaultfd rejects shared shmem mappings. */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

TEST(p3s_userfaultfd_shmem)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	void *mapping = MAP_FAILED;
	int memfd = -1;
	int uffd;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0 &&
	    (errno == EPERM || errno == EOPNOTSUPP || errno == ENOSYS))
		SKIP(return, "userfaultfd is unavailable: %s", strerror(errno));
	ASSERT_GE(uffd, 0);
	ASSERT_EQ(0, ioctl(uffd, UFFDIO_API, &api));

	memfd = memfd_create("userfaultfd-shmem-p3s", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, PROCESS_PAGE_SIZE)) {
		close(uffd);
		if (memfd >= 0)
			close(memfd);
		ASSERT_TRUE(false) TH_LOG("memfd setup failed: %s",
					  strerror(errno));
	}
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, memfd, 0);
	if (mapping == MAP_FAILED) {
		close(memfd);
		close(uffd);
		ASSERT_NE(MAP_FAILED, mapping);
	}

	registration.range.start = (unsigned long)mapping;
	registration.range.len = PROCESS_PAGE_SIZE;
	errno = 0;
	EXPECT_EQ(-1, ioctl(uffd, UFFDIO_REGISTER, &registration));
	EXPECT_EQ(EINVAL, errno);

	munmap(mapping, PROCESS_PAGE_SIZE);
	close(memfd);
	close(uffd);
}

P3S_HARNESS_4KB_MAIN
