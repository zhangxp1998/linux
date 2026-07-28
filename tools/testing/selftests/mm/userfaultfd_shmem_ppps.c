// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define SLICES_PER_NATIVE_PAGE	4UL
#define RACE_ITERATIONS		128UL
#define NATIVE_PAGE_SIZE	(SLICES_PER_NATIVE_PAGE * USER_PAGE_SIZE)
#define MAPPING_SIZE		((2 + RACE_ITERATIONS) * NATIVE_PAGE_SIZE)

struct read_args {
	unsigned char *address;
	unsigned char value;
};

struct copy_args {
	int uffd;
	void *destination;
	const void *source;
	pthread_barrier_t *barrier;
	bool result;
};

static void *read_page(void *data)
{
	struct read_args *args = data;

	args->value = *args->address;
	return NULL;
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
	       (message.arg.pagefault.address & ~(USER_PAGE_SIZE - 1)) ==
	       (unsigned long)address;
}

static bool copy_page(int uffd, void *destination, const void *source)
{
	struct uffdio_copy copy = {
		.src = (unsigned long)source,
		.dst = (unsigned long)destination,
		.len = USER_PAGE_SIZE,
	};

	if (!ioctl(uffd, UFFDIO_COPY, &copy))
		return true;
	ksft_print_msg("UFFDIO_COPY to %p failed: %s, copy=%lld\n",
		       destination, strerror(errno), (long long)copy.copy);
	return false;
}

static void *copy_page_thread(void *data)
{
	struct copy_args *args = data;

	pthread_barrier_wait(args->barrier);
	args->result = copy_page(args->uffd, args->destination, args->source);
	return NULL;
}

static bool race_folio_insertion(int uffd, unsigned char *mapping,
				 unsigned char *source)
{
	for (unsigned long iteration = 0; iteration < RACE_ITERATIONS;
	     iteration++) {
		unsigned long offset = (2 + iteration) * NATIVE_PAGE_SIZE;
		struct copy_args args[SLICES_PER_NATIVE_PAGE];
		pthread_t threads[SLICES_PER_NATIVE_PAGE];
		pthread_barrier_t barrier;
		bool success = true;

		if (pthread_barrier_init(&barrier, NULL,
					 SLICES_PER_NATIVE_PAGE + 1))
			return false;
		for (unsigned long slice = 0; slice < SLICES_PER_NATIVE_PAGE;
		     slice++) {
			args[slice].uffd = uffd;
			args[slice].destination = mapping + offset +
				slice * USER_PAGE_SIZE;
			args[slice].source = source + offset +
				slice * USER_PAGE_SIZE;
			args[slice].barrier = &barrier;
			args[slice].result = false;
			if (pthread_create(&threads[slice], NULL,
					   copy_page_thread, &args[slice]))
				ksft_exit_fail_msg("copy thread creation failed\n");
		}
		pthread_barrier_wait(&barrier);
		for (unsigned long slice = 0; slice < SLICES_PER_NATIVE_PAGE;
		     slice++) {
			if (pthread_join(threads[slice], NULL) ||
			    !args[slice].result ||
			    mapping[offset + slice * USER_PAGE_SIZE] !=
			    source[offset + slice * USER_PAGE_SIZE])
				success = false;
		}
		pthread_barrier_destroy(&barrier);
		if (!success)
			return false;
	}
	return true;
}

static int run_test(void)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	struct uffdio_range unregister_range;
	struct read_args tracked_read;
	struct read_args untracked_read;
	unsigned char *mapping;
	unsigned char *alias;
	unsigned char *source;
	pthread_t tracked_thread;
	pthread_t untracked_thread;
	bool tracked_fault;
	bool tracked_joined;
	bool untracked_fault;
	bool untracked_joined;
	bool untracked_copy;
	bool untouched_zero;
	bool first_copy;
	bool second_copy;
	bool registered;
	bool api_ok;
	int memfd;
	int uffd;

	ksft_print_header();
	ksft_set_plan(13);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0) {
		if (errno == EPERM)
			ksft_exit_skip("userfaultfd is unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd failed: %s\n", strerror(errno));
	}
	api_ok = !ioctl(uffd, UFFDIO_API, &api);
	ksft_test_result(api_ok, "enable the userfaultfd API\n");
	if (!api_ok)
		ksft_exit_fail_msg("UFFDIO_API failed: %s\n", strerror(errno));

	memfd = memfd_create("userfaultfd-shmem-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, MAPPING_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	alias = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     memfd, 0);
	if (alias == MAP_FAILED)
		ksft_exit_fail_msg("shmem alias mmap failed: %s\n",
				   strerror(errno));
	/* Populate the second native folio without creating a UFFD slice mask. */
	alias[SLICES_PER_NATIVE_PAGE * USER_PAGE_SIZE] = 0x55;
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("shmem mmap failed: %s\n", strerror(errno));
	source = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		ksft_exit_fail_msg("source mmap failed: %s\n", strerror(errno));
	for (unsigned long i = 0; i < MAPPING_SIZE / USER_PAGE_SIZE; i++)
		memset(source + i * USER_PAGE_SIZE, 0x41 + i, USER_PAGE_SIZE);

	registration.range.start = (unsigned long)mapping;
	registration.range.len = MAPPING_SIZE;
	registered = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	ksft_test_result(registered, "register the shmem test range\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	first_copy = copy_page(uffd, mapping, source);
	ksft_test_result(first_copy, "copy into the first 4K shmem slice\n");

	tracked_read.address = mapping + USER_PAGE_SIZE;
	if (pthread_create(&tracked_thread, NULL, read_page, &tracked_read))
		ksft_exit_fail_msg("tracked reader creation failed\n");
	tracked_fault = wait_for_fault(uffd, mapping + USER_PAGE_SIZE);
	ksft_test_result(tracked_fault,
			 "unpopulated tracked sibling raises UFFD missing\n");
	second_copy = copy_page(uffd, mapping + USER_PAGE_SIZE,
				source + USER_PAGE_SIZE);
	ksft_test_result(second_copy,
			 "resolve the tracked sibling with UFFDIO_COPY\n");
	tracked_joined = !pthread_join(tracked_thread, NULL);
	ksft_test_result(tracked_joined && tracked_read.value == 0x42 &&
			 mapping[0] == 0x41,
			 "both copied shmem slices retain their contents\n");

	untracked_read.address = mapping +
		(SLICES_PER_NATIVE_PAGE + 1) * USER_PAGE_SIZE;
	if (pthread_create(&untracked_thread, NULL, read_page, &untracked_read))
		ksft_exit_fail_msg("untracked reader creation failed\n");
	untracked_fault = wait_for_fault(uffd, (void *)untracked_read.address);
	ksft_test_result(untracked_fault,
			 "existing folio without a slice mask raises UFFD missing\n");
	untracked_copy = copy_page(uffd, (void *)untracked_read.address,
				   source +
				   (SLICES_PER_NATIVE_PAGE + 1) * USER_PAGE_SIZE);
	ksft_test_result(untracked_copy,
			 "empty slice mask accepts the first UFFDIO_COPY\n");
	untracked_joined = !pthread_join(untracked_thread, NULL);
	ksft_test_result(untracked_joined && untracked_read.value == 0x46 &&
			 alias[SLICES_PER_NATIVE_PAGE * USER_PAGE_SIZE] == 0x55,
			 "pre-existing and copied slices retain their contents\n");

	ksft_test_result(race_folio_insertion(uffd, mapping, source),
			 "concurrent slice copies retry shmem folio insertion races\n");

	unregister_range.start = (unsigned long)mapping;
	unregister_range.len = MAPPING_SIZE;
	ksft_test_result(!ioctl(uffd, UFFDIO_UNREGISTER, &unregister_range),
			 "unregister the shmem range\n");
	untouched_zero = mapping[2 * USER_PAGE_SIZE] == 0 &&
		mapping[3 * USER_PAGE_SIZE] == 0;
	ksft_test_result(untouched_zero,
			 "untouched siblings of a UFFD-allocated folio are zero\n");

	munmap(source, MAPPING_SIZE);
	munmap(mapping, MAPPING_SIZE);
	munmap(alias, MAPPING_SIZE);
	close(memfd);
	close(uffd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "userfaultfd_shmem_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
