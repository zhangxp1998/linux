// SPDX-License-Identifier: GPL-2.0
/*
 * FUTEX_WAKE on a missing 4KB UFFD slice mapped at +4KB inside a PROT_NONE
 * reservation faults in the exact 4KB slice rather than rounding down to the
 * PROT_NONE 16KB boundary.
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

struct fault_handler {
	int fd;
	void *source;
	size_t page_size;
	atomic_bool stop;
	unsigned int events;
	int error;
};

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

TEST(futex_wake_retries_missing_uffd_page)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct fault_handler h = { .page_size = PROCESS_PAGE_SIZE, .fd = -1 };
	void *reservation = MAP_FAILED;
	uintptr_t aligned;
	uint32_t *word = MAP_FAILED;
	pthread_t thread;
	int result, error, saved_errno;

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, reservation);
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	word = mmap((void *)(aligned + PROCESS_PAGE_SIZE), PROCESS_PAGE_SIZE,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	h.source = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (word == MAP_FAILED || h.source == MAP_FAILED) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		if (h.source != MAP_FAILED)
			munmap(h.source, PROCESS_PAGE_SIZE);
		ASSERT_TRUE(false);
	}
	memset(h.source, 0, PROCESS_PAGE_SIZE);
	*word = 0;
	result = syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
	if (result) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		munmap(h.source, PROCESS_PAGE_SIZE);
		ASSERT_EQ(0, result);
	}

	h.fd = syscall(SYS_userfaultfd, O_NONBLOCK | O_CLOEXEC);
	if (h.fd < 0 || ioctl(h.fd, UFFDIO_API, &api)) {
		saved_errno = errno;
		if (h.fd >= 0)
			close(h.fd);
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		munmap(h.source, PROCESS_PAGE_SIZE);
		if (saved_errno == EPERM || saved_errno == ENOSYS ||
		    saved_errno == EACCES)
			SKIP(return, "kernel-fault userfaultfd unavailable: %s",
			     strerror(saved_errno));
		errno = saved_errno;
		ASSERT_TRUE(false);
	}
	if (madvise(word, PROCESS_PAGE_SIZE, MADV_DONTNEED)) {
		close(h.fd);
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		munmap(h.source, PROCESS_PAGE_SIZE);
		ASSERT_TRUE(false);
	}
	reg.range.start = (uintptr_t)word;
	reg.range.len = PROCESS_PAGE_SIZE;
	if (ioctl(h.fd, UFFDIO_REGISTER, &reg)) {
		close(h.fd);
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		munmap(h.source, PROCESS_PAGE_SIZE);
		ASSERT_TRUE(false);
	}
	if (pthread_create(&thread, NULL, resolve_fault, &h)) {
		ioctl(h.fd, UFFDIO_UNREGISTER, &reg.range);
		close(h.fd);
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		munmap(h.source, PROCESS_PAGE_SIZE);
		ASSERT_TRUE(false);
	}

	errno = 0;
	result = syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
	error = errno;
	atomic_store(&h.stop, true);
	pthread_join(thread, NULL);
	ioctl(h.fd, UFFDIO_UNREGISTER, &reg.range);
	ksft_print_msg("pagesize=%zu personality=%#x wake=%d errno=%d events=%u handler=%d\n",
		       h.page_size, personality(0xffffffffUL), result, error,
		       h.events, h.error);
	EXPECT_EQ(0, result);
	EXPECT_GT(h.events, 0U);
	EXPECT_EQ(0, h.error);

	close(h.fd);
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
	munmap(h.source, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
