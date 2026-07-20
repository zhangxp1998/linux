// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "fault_in_ppps.h"
#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define RANGE_SIZE (4 * USER_PAGE_SIZE)
#define RESERVE_SIZE (8 * RANGE_SIZE)

static const char *const operation_names[] = {
	[FAULT_IN_PPPS_WRITEABLE] = "fault_in_writeable",
	[FAULT_IN_PPPS_SAFE_WRITEABLE] = "fault_in_safe_writeable",
	[FAULT_IN_PPPS_READABLE] = "fault_in_readable",
};

static unsigned char *aligned_mapping(void)
{
	unsigned char *reservation;
	unsigned long aligned;
	unsigned char *mapping;

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((unsigned long)reservation + RANGE_SIZE - 1) &
		  ~(RANGE_SIZE - 1);
	mapping = mmap((void *)aligned, RANGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, RESERVE_SIZE);
		return MAP_FAILED;
	}
	if (aligned > (unsigned long)reservation)
		munmap(reservation, aligned - (unsigned long)reservation);
	if (aligned + RANGE_SIZE < (unsigned long)reservation + RESERVE_SIZE)
		munmap((void *)(aligned + RANGE_SIZE),
		       (unsigned long)reservation + RESERVE_SIZE -
		       aligned - RANGE_SIZE);
	return mapping;
}

static bool all_nonresident(unsigned char *mapping)
{
	unsigned char vec[RANGE_SIZE / USER_PAGE_SIZE];
	size_t i;

	if (mincore(mapping, RANGE_SIZE, vec))
		return false;
	for (i = 0; i < sizeof(vec); i++) {
		if (vec[i] & 1)
			return false;
	}
	return true;
}

static bool all_resident(unsigned char *mapping, unsigned char *vec)
{
	size_t i;

	if (mincore(mapping, RANGE_SIZE, vec))
		return false;
	for (i = 0; i < RANGE_SIZE / USER_PAGE_SIZE; i++) {
		vec[i] &= 1;
		if (!vec[i])
			return false;
	}
	return true;
}

static bool run_operation(int fd, unsigned int operation)
{
	unsigned char vec[RANGE_SIZE / USER_PAGE_SIZE] = {};
	struct fault_in_ppps_args request = {};
	unsigned char *mapping;
	bool resident;

	mapping = aligned_mapping();
	if (mapping == MAP_FAILED) {
		ksft_print_msg("mmap failed: %s\n", strerror(errno));
		return false;
	}
	if (madvise(mapping, RANGE_SIZE, MADV_DONTNEED) ||
	    !all_nonresident(mapping)) {
		ksft_print_msg("could not establish nonresident range\n");
		munmap(mapping, RANGE_SIZE);
		return false;
	}

	request.address = (unsigned long)mapping;
	request.length = RANGE_SIZE;
	request.operation = operation;
	if (ioctl(fd, FAULT_IN_PPPS_IOCTL, &request)) {
		ksft_print_msg("%s ioctl failed: %s\n",
			       operation_names[operation], strerror(errno));
		munmap(mapping, RANGE_SIZE);
		return false;
	}
	resident = all_resident(mapping, vec);
	ksft_print_msg("%s: not_faulted=%llu residency=%u,%u,%u,%u\n",
		       operation_names[operation],
		       (unsigned long long)request.not_faulted,
		       vec[0], vec[1], vec[2], vec[3]);
	munmap(mapping, RANGE_SIZE);
	return request.not_faulted == 0 && resident;
}

static void run_test(void)
{
	unsigned int operation;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (sysconf(_SC_PAGESIZE) != USER_PAGE_SIZE)
		ksft_exit_fail_msg("could not enter 4K process mode\n");
	fd = open("/dev/" FAULT_IN_PPPS_DEVICE_NAME, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));
	for (operation = 0; operation < 3; operation++) {
		bool passed = run_operation(fd, operation);

		ksft_test_result(passed, "%s faults every process page\n",
				 operation_names[operation]);
	}
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	int persona;

	if (argc == 2 && !strcmp(argv[1], "--run"))
		run_test();
	if (argc != 1)
		return 1;
	persona = personality(0xffffffffUL);
	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0) {
		ksft_exit_fail_msg("personality failed: %s\n", strerror(errno));
	}
	execl("/proc/self/exe", "fault_in_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}
