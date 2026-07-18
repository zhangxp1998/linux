// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process can register a shmem 4K slice beyond the memfd's EOF
 * with userfaultfd, but UFFDIO_COPY into it is rejected (EFAULT/ENOMEM).
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest.h"

#define MAPPING_SIZE	(2 * PROCESS_PAGE_SIZE)

static int run_test(void)
{
	struct uffdio_register registration = {
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
	};
	struct uffdio_copy copy = {};
	unsigned char *mapping;
	unsigned char *source;
	bool rejected;
	bool registered;
	bool api_ok;
	int memfd;
	int uffd;
	int result;
	int error;

	ksft_print_header();
	ksft_set_plan(3);

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

	memfd = memfd_create("userfaultfd-eof-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, 0);
	source = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || source == MAP_FAILED)
		ksft_exit_fail_msg("mmap setup failed: %s\n", strerror(errno));
	memset(source, 0x45, PROCESS_PAGE_SIZE);

	registration.range.start = (unsigned long)mapping + PROCESS_PAGE_SIZE;
	registration.range.len = PROCESS_PAGE_SIZE;
	registered = !ioctl(uffd, UFFDIO_REGISTER, &registration);
	ksft_test_result(registered, "register a shmem slice beyond EOF\n");
	if (!registered)
		ksft_exit_fail_msg("UFFDIO_REGISTER failed: %s\n",
				   strerror(errno));

	copy.src = (unsigned long)source;
	copy.dst = (unsigned long)mapping + PROCESS_PAGE_SIZE;
	copy.len = PROCESS_PAGE_SIZE;
	result = ioctl(uffd, UFFDIO_COPY, &copy);
	error = errno;
	rejected = result == -1 && (error == EFAULT || error == ENOMEM) &&
		   copy.copy == -error;
	ksft_test_result(rejected, "reject UFFDIO_COPY beyond the file size\n");
	ksft_print_msg("UFFDIO_COPY result=%d errno=%d copy=%lld\n",
		       result, error, (long long)copy.copy);

	munmap(source, PROCESS_PAGE_SIZE);
	munmap(mapping, MAPPING_SIZE);
	close(memfd);
	close(uffd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
