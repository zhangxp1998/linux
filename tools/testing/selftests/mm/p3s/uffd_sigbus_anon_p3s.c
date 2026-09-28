// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"
#include "vm_util.h"
#include "p3s_harness.h"

#define SLICES 4
#define ROUNDS 1000

static unsigned char *destination;
static unsigned char *source;
static int uffd = -1;

static void sigbus_handler(int sig, siginfo_t *info, void *context)
{
	unsigned long fault = (unsigned long)info->si_addr & ~(PROCESS_PAGE_SIZE - 1);
	unsigned long index = (fault - (unsigned long)destination) / PROCESS_PAGE_SIZE;
	struct uffdio_copy copy = {
		.dst = fault,
		.src = (unsigned long)source + index * PROCESS_PAGE_SIZE,
		.len = PROCESS_PAGE_SIZE,
	};

	(void)sig;
	(void)context;
	if (fault < (unsigned long)destination || index >= SLICES ||
	    ioctl(uffd, UFFDIO_COPY, &copy) || copy.copy != PROCESS_PAGE_SIZE)
		_exit(KSFT_FAIL);
}

TEST(p3s_uffd_sigbus_anon)
{
	static const unsigned int order[SLICES] = { 2, 0, 3, 1 };
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_SIGBUS,
	};
	struct sigaction action = {
		.sa_sigaction = sigbus_handler,
		.sa_flags = SA_SIGINFO | SA_NODEFER,
	};
	unsigned int round, slice;
	bool passed = true;

	uffd = syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (uffd < 0 &&
	    (errno == EPERM || errno == EOPNOTSUPP || errno == ENOSYS))
		SKIP(return, "userfaultfd is unavailable: %s", strerror(errno));
	ASSERT_GE(uffd, 0);
	destination = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	source = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, destination);
	ASSERT_NE(MAP_FAILED, source);
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
	ASSERT_EQ(0, ioctl(uffd, UFFDIO_API, &api));
	ASSERT_EQ(0, uffd_register(uffd, destination, NATIVE_PAGE_SIZE,
				   true, false, false));

	for (round = 0; round < ROUNDS && passed; round++) {
		for (slice = 0; slice < SLICES; slice++) {
			memset(source + slice * PROCESS_PAGE_SIZE,
			       0x31 + slice + (round & 7), PROCESS_PAGE_SIZE);
			memset(destination + slice * PROCESS_PAGE_SIZE,
			       0xa1 + slice, PROCESS_PAGE_SIZE);
		}
		ASSERT_EQ(0, madvise(destination, NATIVE_PAGE_SIZE,
				     MADV_DONTNEED));
		for (slice = 0; slice < SLICES; slice++) {
			unsigned int victim = order[slice];
			unsigned char expected = 0x31 + victim + (round & 7);
			unsigned char *page = destination + victim * PROCESS_PAGE_SIZE;
			size_t i;

			if (page[0] != expected) {
				passed = false;
				break;
			}
			for (i = 1; i < PROCESS_PAGE_SIZE; i++) {
				if (page[i] != expected) {
					ksft_print_msg("round %u slice %u offset %zu: %#x != %#x\n",
						       round, victim, i, page[i], expected);
					passed = false;
					break;
				}
			}
			if (!passed)
				break;
		}
	}
	EXPECT_TRUE(passed);
	close(uffd);
	uffd = -1;
	munmap(destination, NATIVE_PAGE_SIZE);
	munmap(source, NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
