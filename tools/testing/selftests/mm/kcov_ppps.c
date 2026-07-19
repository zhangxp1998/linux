// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kcov.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
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

#define PROCESS_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define COVER_ENTRIES (512UL << 10)
#define COVER_BYTES (COVER_ENTRIES * sizeof(unsigned long))

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_coverage(const unsigned long *cover, unsigned long count)
{
	unsigned long checksum = 0;
	unsigned long i;

	if (sigsetjmp(fault_environment, 1))
		return false;
	for (i = 1; i <= count; i++)
		checksum ^= cover[i];
	return checksum != 0;
}

static int run_test(void)
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

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (sysconf(_SC_PAGESIZE) != PROCESS_PAGE_SIZE)
		ksft_exit_skip("4K process pages are unavailable\n");

	fd = open("/sys/kernel/debug/kcov", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open KCOV debugfs file\n");
	if (fd < 0)
		ksft_exit_fail_msg("KCOV open failed: %s\n", strerror(errno));
	if (ioctl(fd, KCOV_INIT_TRACE, COVER_ENTRIES))
		ksft_exit_fail_msg("KCOV_INIT_TRACE failed: %s\n",
				   strerror(errno));

	reservation = mmap(NULL, COVER_BYTES + 2 * NATIVE_PAGE_SIZE,
			   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("guard reservation failed: %s\n",
				   strerror(errno));
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	cover = mmap((void *)(aligned + PROCESS_PAGE_SIZE), COVER_BYTES,
		     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	ksft_test_result(cover == (void *)(aligned + PROCESS_PAGE_SIZE),
			 "map KCOV after a 4K guard slice\n");
	if (cover == MAP_FAILED)
		ksft_exit_fail_msg("KCOV mmap failed: %s\n", strerror(errno));

	if (ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC))
		ksft_exit_fail_msg("KCOV_ENABLE failed: %s\n", strerror(errno));
	__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);
	for (i = 0; i < 64; i++)
		syscall(__NR_getpid);
	if (ioctl(fd, KCOV_DISABLE, 0))
		ksft_exit_fail_msg("KCOV_DISABLE failed: %s\n", strerror(errno));

	count = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
	ksft_test_result(count > PROCESS_PAGE_SIZE / sizeof(*cover) &&
			 count < COVER_ENTRIES,
			 "collect coverage across multiple 4K slices\n");

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL) ||
	    sigaction(SIGSEGV, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	ksft_test_result(count < COVER_ENTRIES && read_coverage(cover, count),
			 "read every KCOV entry without a mapping fault\n");

	munmap(reservation, COVER_BYTES + 2 * NATIVE_PAGE_SIZE);
	close(fd);
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
	execl("/proc/self/exe", "kcov_ppps", "--run", NULL);
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
