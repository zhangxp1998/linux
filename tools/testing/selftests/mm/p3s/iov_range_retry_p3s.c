// SPDX-License-Identifier: GPL-2.0
/* Existing iov APIs must preserve bytes, runs and UFFD retries in both modes. */
#define _GNU_SOURCE
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "p3s_test.h"
#include "iov_range_retry_p3s.h"

#define BULK_SIZE (1024 * 1024)

struct fault_handler {
	int fd;
	void *source;
	size_t page_size;
	atomic_bool stop;
	unsigned int events;
	int error;
};

static uint32_t checksum(const unsigned char *data, size_t length)
{
	uint32_t value = 2166136261U;
	size_t i;

	for (i = 0; i < length; i++) {
		value ^= data[i];
		value *= 16777619U;
	}
	return value;
}

static void *resolve_fault(void *argument)
{
	struct fault_handler *h = argument;
	struct pollfd p = { .fd = h->fd, .events = POLLIN };

	while (!atomic_load(&h->stop)) {
		struct uffd_msg msg;
		struct uffdio_copy copy = {
			.src = (uintptr_t)h->source,
			.len = h->page_size,
		};
		int ret = poll(&p, 1, 100);

		if (ret < 0 && errno != EINTR) {
			h->error = errno;
			break;
		}
		if (ret <= 0)
			continue;
		if (read(h->fd, &msg, sizeof(msg)) != sizeof(msg) ||
		    msg.event != UFFD_EVENT_PAGEFAULT) {
			h->error = EPROTO;
			break;
		}
		h->events++;
		copy.dst = msg.arg.pagefault.address & ~(h->page_size - 1);
		if (ioctl(h->fd, UFFDIO_COPY, &copy)) {
			h->error = errno;
			break;
		}
	}
	return NULL;
}

static bool range_matches(int fd, void *address, size_t length, bool pin)
{
	struct iov_range_retry_p3s_args r = {
		.address = (uintptr_t)address, .length = length, .pin = pin,
	};
	uint32_t expected = checksum(address, length);
	int ret = ioctl(fd, IOV_RANGE_RETRY_P3S_IOCTL, &r);

	if (ret || r.result != (int64_t)length || r.checksum != expected) {
		ksft_print_msg("%s: ret=%d bytes=%lld/%zu offset=%llu checksum=%#x/%#x\n",
			       pin ? "pin" : "get", ret, (long long)r.result,
			       length, (unsigned long long)r.offset,
			       r.checksum, expected);
		return false;
	}
	return true;
}

static int check_uffd(int fd, void *destination, void *source, bool pin)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct fault_handler h = { .source = source, .page_size = getpagesize() };
	struct iov_range_retry_p3s_args r = {
		.address = (uintptr_t)destination, .length = h.page_size, .pin = pin,
	};
	pthread_t thread;
	int ret;

	h.fd = syscall(SYS_userfaultfd, O_NONBLOCK | O_CLOEXEC);
	if (h.fd < 0 || ioctl(h.fd, UFFDIO_API, &api)) {
		if (h.fd >= 0)
			close(h.fd);
		return KSFT_SKIP;
	}
	if (madvise(destination, NATIVE_PAGE_SIZE, MADV_DONTNEED))
		goto fail;
	if (uffd_register(h.fd, destination, NATIVE_PAGE_SIZE,
			  true, false, false))
		goto fail;
	if (pthread_create(&thread, NULL, resolve_fault, &h))
		goto unregister;
	ret = ioctl(fd, IOV_RANGE_RETRY_P3S_IOCTL, &r);
	atomic_store(&h.stop, true);
	pthread_join(thread, NULL);
	uffd_unregister(h.fd, destination, NATIVE_PAGE_SIZE);
	close(h.fd);
	if (!ret && r.result == (int64_t)h.page_size && h.events &&
	    !h.error && r.checksum == checksum(source, h.page_size))
		return KSFT_PASS;
	ksft_print_msg("%s UFFD retry: ret=%d bytes=%lld events=%u handler=%d\n",
		       pin ? "pin" : "get", ret, (long long)r.result,
		       h.events, h.error);
	return KSFT_FAIL;
unregister:
	uffd_unregister(h.fd, destination, NATIVE_PAGE_SIZE);
fail:
	ksft_print_msg("%s UFFD setup: %s\n", pin ? "pin" : "get",
		       strerror(errno));
	close(h.fd);
	return KSFT_FAIL;
}

FIXTURE(iov_range) {
	unsigned char *allocation;
	unsigned char *bulk;
	unsigned char *destination;
	int fd;
};

FIXTURE_SETUP(iov_range)
{
	size_t i;

	self->fd = open("/dev/iov_range_retry_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "iov_range_retry fixture unavailable");
	ASSERT_GE(self->fd, 0);
	self->allocation = mmap(NULL, BULK_SIZE + NATIVE_PAGE_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->allocation, MAP_FAILED);
	self->destination = mmap(NULL, NATIVE_PAGE_SIZE,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->destination, MAP_FAILED);
	self->bulk = (void *)(((uintptr_t)self->allocation + NATIVE_PAGE_SIZE - 1) &
			      ~(NATIVE_PAGE_SIZE - 1));
	for (i = 0; i < BULK_SIZE; i++)
		self->bulk[i] = (i * 31 + (i >> 12) * 17 + (i >> 14)) & 0xff;
}

FIXTURE_TEARDOWN(iov_range)
{
	munmap(self->allocation, BULK_SIZE + NATIVE_PAGE_SIZE);
	munmap(self->destination, NATIVE_PAGE_SIZE);
	close(self->fd);
}

TEST_F(iov_range, get_one_megabyte)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk, BULK_SIZE, false));
}

TEST_F(iov_range, pin_one_megabyte)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk, BULK_SIZE, true));
}

TEST_F(iov_range, get_unaligned_multi_page_range)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk + 17,
				  2 * PROCESS_PAGE_SIZE + 46, false));
}

TEST_F(iov_range, pin_unaligned_multi_page_range)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk + 17,
				  2 * PROCESS_PAGE_SIZE + 46, true));
}

TEST_F(iov_range, get_slice_boundary_range)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk + PROCESS_PAGE_SIZE - 1,
				  3 * PROCESS_PAGE_SIZE + 17, false));
}

TEST_F(iov_range, pin_slice_boundary_range)
{
	EXPECT_TRUE(range_matches(self->fd, self->bulk + PROCESS_PAGE_SIZE - 1,
				  3 * PROCESS_PAGE_SIZE + 17, true));
}

TEST_F(iov_range, get_retries_userfaultfd_fault)
{
	int result = check_uffd(self->fd, self->destination, self->bulk, false);

	if (result == KSFT_SKIP)
		SKIP(return, "kernel-fault userfaultfd unavailable");
	EXPECT_EQ(KSFT_PASS, result);
}

TEST_F(iov_range, pin_retries_userfaultfd_fault)
{
	int result = check_uffd(self->fd, self->destination, self->bulk, true);

	if (result == KSFT_SKIP)
		SKIP(return, "kernel-fault userfaultfd unavailable");
	EXPECT_EQ(KSFT_PASS, result);
}

P3S_HARNESS_4KB_MAIN
