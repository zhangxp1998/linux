// SPDX-License-Identifier: GPL-2.0
/*
 * UFFDIO_COPY in a 4K compat process retries after faulting on its source:
 * a destination replaced meanwhile by a file slice at a different PPPS
 * offset receives the data in that new slice, and a shmem self-copy retry
 * returns EEXIST.
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

#include "kselftest_ppps.h"

struct copy_thread_args {
	struct uffdio_copy copy;
	int uffd;
	int error;
	int result;
};

static void *copy_thread(void *data)
{
	struct copy_thread_args *args = data;

	args->result = ioctl(args->uffd, UFFDIO_COPY, &args->copy);
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

static bool register_missing(int uffd, void *address)
{
	struct uffdio_register registration = {
		.range = {
			.start = (unsigned long)address,
			.len = PROCESS_PAGE_SIZE,
		},
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};

	return !ioctl(uffd, UFFDIO_REGISTER, &registration);
}

static bool wait_for_source_fault(int uffd, void *source)
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
	       (unsigned long)source;
}

static bool resolve_source_fault(int uffd, void *source, void *data)
{
	struct uffdio_copy copy = {
		.src = (unsigned long)data,
		.dst = (unsigned long)source,
		.len = PROCESS_PAGE_SIZE,
	};

	return !ioctl(uffd, UFFDIO_COPY, &copy);
}

static void test_shmem_self_copy_retry(void)
{
	struct copy_thread_args thread_args = {};
	struct timespec deadline;
	unsigned char *source_data;
	void *mapping;
	pthread_t thread;
	bool registered;
	bool resolved;
	bool joined = false;
	bool started;
	int uffd;
	int memfd;

	uffd = open_userfaultfd();
	memfd = memfd_create("userfaultfd-self-copy-ppps", MFD_CLOEXEC);
	if (memfd >= 0 && ftruncate(memfd, PROCESS_PAGE_SIZE)) {
		close(memfd);
		memfd = -1;
	}
	mapping = memfd < 0 ? MAP_FAILED :
		mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, memfd, 0);
	source_data = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(uffd >= 0 && memfd >= 0 &&
			 mapping != MAP_FAILED && source_data != MAP_FAILED,
			 "set up shared self-copy mapping\n");
	if (uffd < 0 || memfd < 0 || mapping == MAP_FAILED ||
	    source_data == MAP_FAILED)
		ksft_exit_fail_msg("self-copy setup failed: %s\n", strerror(errno));
	memset(source_data, 0x6b, PROCESS_PAGE_SIZE);

	registered = register_missing(uffd, mapping);
	ksft_test_result(registered, "register self-copy mapping\n");
	if (!registered)
		ksft_exit_fail_msg("self-copy register failed: %s\n",
				   strerror(errno));
	thread_args.uffd = uffd;
	thread_args.copy.src = (unsigned long)mapping;
	thread_args.copy.dst = (unsigned long)mapping;
	thread_args.copy.len = PROCESS_PAGE_SIZE;
	started = !pthread_create(&thread, NULL, copy_thread, &thread_args);
	ksft_test_result(started, "start faulting shmem self-copy\n");
	if (!started)
		ksft_exit_fail_msg("self-copy pthread_create failed\n");
	ksft_test_result(wait_for_source_fault(uffd, mapping),
			 "observe self-copy source fault\n");

	resolved = resolve_source_fault(uffd, mapping, source_data);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	if (resolved)
		joined = !pthread_timedjoin_np(thread, NULL, &deadline);
	ksft_test_result(resolved && joined && thread_args.result == -1 &&
			 thread_args.error == EEXIST &&
			 thread_args.copy.copy == -EEXIST &&
			 !memcmp(mapping, source_data, PROCESS_PAGE_SIZE),
			 "self-copy retry returns EEXIST without a kernel BUG\n");

	munmap(source_data, PROCESS_PAGE_SIZE);
	munmap(mapping, PROCESS_PAGE_SIZE);
	close(memfd);
	close(uffd);
}

static int run_test(void)
{
	struct copy_thread_args thread_args = {};
	struct timespec deadline;
	unsigned char *source_data;
	void *replacement;
	void *destination;
	void *source;
	pthread_t thread;
	bool source_resolved;
	bool replacement_ready;
	bool source_fault;
	bool registered;
	bool apis_ok;
	bool thread_started;
	bool thread_joined;
	int destination_uffd;
	int source_uffd;
	int memfd;

	ksft_print_header();
	ksft_set_plan(13);

	destination_uffd = open_userfaultfd();
	source_uffd = open_userfaultfd();
	apis_ok = destination_uffd >= 0 && source_uffd >= 0;
	ksft_test_result(apis_ok, "enable both userfaultfd APIs\n");
	if (!apis_ok) {
		if (errno == EPERM)
			ksft_exit_skip("userfaultfd is unavailable\n");
		ksft_exit_fail_msg("userfaultfd setup failed: %s\n",
				   strerror(errno));
	}

	memfd = memfd_create("userfaultfd-retry-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, 2 * PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	destination = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, memfd, 0);
	source = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	source_data = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (destination == MAP_FAILED || source == MAP_FAILED ||
	    source_data == MAP_FAILED)
		ksft_exit_fail_msg("mmap setup failed: %s\n", strerror(errno));
	memset(source_data, 0x5a, PROCESS_PAGE_SIZE);

	registered = register_missing(destination_uffd, destination) &&
		     register_missing(source_uffd, source);
	ksft_test_result(registered, "register source and destination ranges\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	thread_args.uffd = destination_uffd;
	thread_args.copy.src = (unsigned long)source;
	thread_args.copy.dst = (unsigned long)destination;
	thread_args.copy.len = PROCESS_PAGE_SIZE;
	thread_started = !pthread_create(&thread, NULL, copy_thread, &thread_args);
	ksft_test_result(thread_started, "start a faulting UFFDIO_COPY\n");
	if (!thread_started)
		ksft_exit_fail_msg("pthread_create failed\n");

	source_fault = wait_for_source_fault(source_uffd, source);
	ksft_test_result(source_fault,
			 "UFFDIO_COPY retries after faulting on its source\n");
	if (!source_fault)
		ksft_exit_fail_msg("did not observe the source fault\n");

	replacement = mmap(destination, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED, memfd, PROCESS_PAGE_SIZE);
	replacement_ready = replacement == destination &&
			    register_missing(destination_uffd, destination);
	ksft_test_result(replacement_ready,
			 "replace the destination with a new file slice\n");

	source_resolved = resolve_source_fault(source_uffd, source, source_data);
	ksft_test_result(source_resolved, "resolve the blocked source fault\n");
	if (!source_resolved)
		ksft_exit_fail_msg("failed to resolve source fault: %s\n",
				   strerror(errno));

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	thread_joined = !pthread_timedjoin_np(thread, NULL, &deadline);
	ksft_test_result(thread_joined, "the UFFDIO_COPY retry completes\n");
	ksft_test_result(thread_joined && thread_args.result == 0 &&
			 thread_args.copy.copy == PROCESS_PAGE_SIZE &&
			 !memcmp(destination, source_data, PROCESS_PAGE_SIZE),
			 "copy into a replacement with a different PPPS slice offset\n");
	if (thread_joined)
		ksft_print_msg("UFFDIO_COPY result=%d errno=%d copy=%lld\n",
			       thread_args.result, thread_args.error,
			       (long long)thread_args.copy.copy);

	munmap(source_data, PROCESS_PAGE_SIZE);
	munmap(source, PROCESS_PAGE_SIZE);
	munmap(destination, PROCESS_PAGE_SIZE);
	close(memfd);
	close(source_uffd);
	close(destination_uffd);
	test_shmem_self_copy_retry();
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
