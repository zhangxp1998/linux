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
	unsigned char *destination;
	unsigned char *source;
	pthread_t thread;
	void *moved;
	int uffd;

	EXPECT_TRUE((SOURCE_ADDRESS & (NATIVE_PAGE_SIZE - 1)) == PROCESS_PAGE_SIZE &&
			 !(DESTINATION_ADDRESS & (PROCESS_PAGE_SIZE - 1)) &&
			 (DESTINATION_ADDRESS & (NATIVE_PAGE_SIZE - 1)));

	source = mmap((void *)SOURCE_ADDRESS, PROCESS_PAGE_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	destination = mmap((void *)DESTINATION_ADDRESS, PROCESS_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	if (source == MAP_FAILED || destination == MAP_FAILED)
		ksft_exit_fail_msg("fixed mmap failed: %s\n", strerror(errno));
	memset(source, TEST_VALUE, PROCESS_PAGE_SIZE);

	uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK |
		       UFFD_USER_MODE_ONLY);
	if (uffd < 0) {
		if (errno == EPERM || errno == EOPNOTSUPP)
			SKIP(exit(KSFT_SKIP), "userfaultfd unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd failed: %s\n", strerror(errno));
	}
	if (ioctl(uffd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API failed: %s\n", strerror(errno));
	EXPECT_TRUE(api.features & UFFD_FEATURE_EVENT_REMAP);
	if (ioctl(uffd, UFFDIO_REGISTER, &registration))
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	monitor.uffd = uffd;
	if (pthread_create(&thread, NULL, monitor_remap, &monitor))
		ksft_exit_fail_msg("pthread_create failed\n");
	moved = mremap(source, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, destination);
	EXPECT_TRUE(moved == destination);
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
	munmap(destination, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_COMPAT_MAIN
