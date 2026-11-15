// SPDX-License-Identifier: GPL-2.0
/*
 * An unlockable GUP over two 4K pages of a compat process, the second one
 * userfaultfd-missing, retries at the missing 4K page (not the native-page
 * shifted address) and ends up pinning both pages.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

#include "gup_retry_p3s.h"
#include "p3s_test.h"
#include "p3s_harness.h"

#define RESERVE_SIZE	(4 * NATIVE_PAGE_SIZE)

#ifdef __BIONIC__
static int pthread_timedjoin_np(pthread_t thread, void **retval,
				const struct timespec *deadline)
{
	(void)deadline;
	return pthread_join(thread, retval);
}
#endif

struct ioctl_thread_args {
	struct gup_retry_p3s_args request;
	int fd;
	int result;
	int error;
};

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static void *ioctl_thread(void *data)
{
	struct ioctl_thread_args *args = data;

	errno = 0;
	args->result = ioctl(args->fd, GUP_RETRY_P3S_IOCTL, &args->request);
	args->error = errno;
	return NULL;
}

static int open_userfaultfd(void)
{
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

	if (uffd < 0)
		return -1;
	if (ioctl(uffd, UFFDIO_API, &api)) {
		close(uffd);
		return -1;
	}
	return uffd;
}

static bool wait_for_fault(int uffd, void *address)
{
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	struct uffd_msg message;

	if (poll(&pollfd, 1, 5000) != 1)
		return false;
	if (read(uffd, &message, sizeof(message)) != sizeof(message))
		return false;
	return message.event == UFFD_EVENT_PAGEFAULT &&
	       (message.arg.pagefault.address & ~(PROCESS_PAGE_SIZE - 1)) ==
	       (unsigned long)address;
}

static bool resolve_fault(int uffd, void *address, void *source)
{
	struct uffdio_copy copy = {
		.src = (unsigned long)source,
		.dst = (unsigned long)address,
		.len = PROCESS_PAGE_SIZE,
	};

	return !ioctl(uffd, UFFDIO_COPY, &copy);
}

TEST(p3s_gup_retry)
{
	struct ioctl_thread_args args = {};
	struct timespec deadline;
	unsigned char *source;
	unsigned char *target;
	unsigned char *reserve;
	pthread_t thread;
	bool faulted;
	bool joined;
	bool resolved;
	bool started;
	int device_fd;
	int uffd;

	device_fd = p3s_open_fixture("/dev/gup_retry_p3s", O_RDWR);
	if (device_fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "gup_retry fixture unavailable");
	ASSERT_GE(device_fd, 0);
	uffd = open_userfaultfd();
	if (uffd < 0 && (errno == EPERM || errno == ENOSYS))
		SKIP(return, "kernel-fault userfaultfd unavailable");
	ASSERT_GE(uffd, 0);

	reserve = mmap(NULL, RESERVE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	source = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reserve == MAP_FAILED || source == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	target = (void *)align_up((uintptr_t)reserve, NATIVE_PAGE_SIZE);
	EXPECT_TRUE(target + NATIVE_PAGE_SIZE + PROCESS_PAGE_SIZE <=
		    reserve + RESERVE_SIZE);

	memset(source, 0x22, PROCESS_PAGE_SIZE);
	ASSERT_EQ(0, uffd_register(uffd, target + PROCESS_PAGE_SIZE,
				   PROCESS_PAGE_SIZE, true, false, false));
	memset(target, 0x11, PROCESS_PAGE_SIZE);
	memset(target + NATIVE_PAGE_SIZE, 0x33, PROCESS_PAGE_SIZE);

	args.fd = device_fd;
	args.request.address = (uintptr_t)target;
	started = !pthread_create(&thread, NULL, ioctl_thread, &args);
	EXPECT_TRUE(started);
	if (!started)
		ksft_exit_fail_msg("pthread_create failed\n");

	faulted = wait_for_fault(uffd, target + PROCESS_PAGE_SIZE);
	EXPECT_TRUE(faulted);
	if (!faulted)
		ksft_exit_fail_msg("did not observe the expected fault\n");

	resolved = resolve_fault(uffd, target + PROCESS_PAGE_SIZE, source);
	EXPECT_TRUE(resolved);
	if (!resolved)
		ksft_exit_fail_msg("UFFDIO_COPY failed: %s\n", strerror(errno));

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	joined = !pthread_timedjoin_np(thread, NULL, &deadline);
	EXPECT_TRUE(joined && !args.result &&
		    args.request.nr_pinned == 2);
	if (joined)
		ksft_print_msg("ioctl result=%d errno=%d pinned=%lld match=%u\n",
			       args.result, args.error,
			       (long long)args.request.nr_pinned,
			       args.request.second_matches_expected);

	EXPECT_TRUE(joined && args.request.second_matches_expected);

	EXPECT_EQ(0, uffd_unregister(uffd, target + PROCESS_PAGE_SIZE,
				     PROCESS_PAGE_SIZE));
	munmap(source, PROCESS_PAGE_SIZE);
	munmap(reserve, RESERVE_SIZE);
	close(uffd);
	close(device_fd);
}

P3S_HARNESS_4KB_MAIN
