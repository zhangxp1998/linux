// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process maps the KCOV coverage buffer at a 4KB offset from a
 * native-page boundary, collects coverage spanning several 4KB slices and
 * reads every entry back without a mapping fault.
 */
#define _GNU_SOURCE

#include <linux/kcov.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

#define COVER_ENTRIES (512UL << 10)
#define COVER_BYTES (COVER_ENTRIES * sizeof(unsigned long))

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_coverage(const unsigned long *cover, unsigned long count)
{
	unsigned long i;

	if (sigsetjmp(fault_environment, 1))
		return false;
	for (i = 1; i <= count; i++) {
		unsigned long pc = cover[i];

		if (!pc || pc == count)
			return false;
	}
	return true;
}

TEST(p3s_kcov)
{
	struct sigaction action = {
		.sa_handler = fault_handler,
	};
	unsigned long *cover;
	unsigned long count;
	unsigned long aligned;
	void *reservation;
	unsigned int i;
	int fd;

	fd = p3s_open_fixture("/sys/kernel/debug/kcov", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "KCOV debugfs fixture unavailable");
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ioctl(fd, KCOV_INIT_TRACE, COVER_ENTRIES));

	reservation = mmap(NULL, COVER_BYTES + 2 * NATIVE_PAGE_SIZE,
			   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, reservation);
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	cover = mmap((void *)(aligned + PROCESS_PAGE_SIZE), COVER_BYTES,
		     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	ASSERT_EQ((void *)(aligned + PROCESS_PAGE_SIZE), cover);

	ASSERT_EQ(0, ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC));
	__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);
	for (i = 0; i < 64; i++)
		syscall(__NR_getpid);
	ASSERT_EQ(0, ioctl(fd, KCOV_DISABLE, 0));

	count = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
	if (!count) {
		munmap(reservation, COVER_BYTES + 2 * NATIVE_PAGE_SIZE);
		close(fd);
		SKIP(return, "KCOV syscall path is uninstrumented");
	}
	EXPECT_TRUE(count > PROCESS_PAGE_SIZE / sizeof(*cover) &&
		    count < COVER_ENTRIES);

	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	EXPECT_TRUE(count < COVER_ENTRIES && read_coverage(cover, count));

	munmap(reservation, COVER_BYTES + 2 * NATIVE_PAGE_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
