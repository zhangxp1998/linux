// SPDX-License-Identifier: GPL-2.0
/*
 * mremap() of a userfaultfd-registered 4K page to a 4K-only-aligned address
 * in a compat process delivers an exact UFFD_EVENT_REMAP and preserves the
 * moved contents.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define SOURCE_ADDRESS		(0x20000000UL + PROCESS_PAGE_SIZE)
#define DESTINATION_ADDRESS	(0x30000000UL + PROCESS_PAGE_SIZE)
#define TEST_VALUE		0x5a

struct monitor_result {
	int uffd;
	int poll_result;
	int read_result;
	int error;
	struct uffd_msg message;
};

static void *monitor_remap(void *arg)
{
	struct monitor_result *result = arg;
	struct pollfd pollfd = {
		.fd = result->uffd,
		.events = POLLIN,
	};

	result->poll_result = poll(&pollfd, 1, 2000);
	if (result->poll_result <= 0) {
		result->error = errno;
		return NULL;
	}
	result->read_result = read(result->uffd, &result->message,
				   sizeof(result->message));
	if (result->read_result < 0)
		result->error = errno;
	return NULL;
}

TEST(p3s_userfaultfd_remap)
{
	struct uffdio_register registration = {
		.range = {
			.start = SOURCE_ADDRESS,
			.len = PROCESS_PAGE_SIZE,
		},
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_EVENT_REMAP,
	};
	struct monitor_result monitor = {};
	unsigned char *destination = MAP_FAILED;
	unsigned char *source = MAP_FAILED;
	pthread_t thread;
	void *moved;
	int uffd = -1;

	uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK |
		       UFFD_USER_MODE_ONLY);
	if (uffd < 0 &&
	    (errno == EPERM || errno == EOPNOTSUPP || errno == ENOSYS))
		SKIP(return, "userfaultfd unavailable: %s", strerror(errno));
	ASSERT_GE(uffd, 0);
	ASSERT_EQ(0, ioctl(uffd, UFFDIO_API, &api));
	ASSERT_TRUE(api.features & UFFD_FEATURE_EVENT_REMAP);

	source = mmap((void *)SOURCE_ADDRESS, PROCESS_PAGE_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	destination = mmap((void *)DESTINATION_ADDRESS, PROCESS_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	if (source == MAP_FAILED || destination == MAP_FAILED) {
		if (source != MAP_FAILED)
			munmap(source, PROCESS_PAGE_SIZE);
		if (destination != MAP_FAILED)
			munmap(destination, PROCESS_PAGE_SIZE);
		close(uffd);
		ASSERT_TRUE(false) TH_LOG("fixed mmap failed: %s",
					  strerror(errno));
	}
	memset(source, TEST_VALUE, PROCESS_PAGE_SIZE);
	ASSERT_EQ(0, ioctl(uffd, UFFDIO_REGISTER, &registration));

	monitor.uffd = uffd;
	ASSERT_EQ(0, pthread_create(&thread, NULL, monitor_remap, &monitor));
	moved = mremap(source, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, destination);
	EXPECT_EQ(destination, moved);
	pthread_join(thread, NULL);
	EXPECT_TRUE(monitor.poll_result == 1 &&
			 monitor.read_result == sizeof(monitor.message) &&
			 monitor.message.event == UFFD_EVENT_REMAP &&
			 monitor.message.arg.remap.from == SOURCE_ADDRESS &&
			 monitor.message.arg.remap.to == DESTINATION_ADDRESS &&
			 monitor.message.arg.remap.len == PROCESS_PAGE_SIZE);
	EXPECT_TRUE(moved == destination &&
			 destination[0] == TEST_VALUE &&
			 destination[PROCESS_PAGE_SIZE - 1] == TEST_VALUE);

	close(uffd);
	munmap(moved == destination ? destination : source, PROCESS_PAGE_SIZE);
	if (moved != destination)
		munmap(destination, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
