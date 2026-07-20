// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../kselftest.h"
#include "vm_iomap_memory_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_16K_SIZE	(4 * USER_PAGE_SIZE)
#define PRIVATE_RESERVATION_SIZE (3 * NATIVE_16K_SIZE)
#define FIRST_MARKER	0x71
#define COW_MARKER	0xa5
#define COW_OFFSET	37

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

static bool mapping_has_values(const unsigned char *mapping,
			       unsigned long changed_offset)
{
	unsigned long offset;

	for (offset = 0; offset < USER_PAGE_SIZE; offset++) {
		unsigned char expected = offset == changed_offset ?
			COW_MARKER : FIRST_MARKER;

		if (mapping[offset] != expected) {
			ksft_print_msg("offset %lu contains %#x instead of %#x\n",
				       offset, mapping[offset], expected);
			return false;
		}
	}
	return true;
}

static int run_test(void)
{
	struct sigaction action = {
		.sa_handler = fault_handler,
	};
	unsigned char *reservation;
	unsigned char *mapping;
	unsigned char value = 0;
	uintptr_t target_address;
	bool guards_fault = true;
	bool mapping_ok;
	unsigned int guard;
	void *past_end;
	int fd;

	ksft_print_header();
	ksft_set_plan(12);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/" VM_IOMAP_MEMORY_PPPS_DEVICE_NAME,
		  O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the vm_iomap_memory test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	reservation = mmap(NULL, NATIVE_16K_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(reservation != MAP_FAILED,
			 "reserve one native 16K range as PROT_NONE\n");
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("guard reservation failed: %s\n",
				   strerror(errno));

	mapping = mmap(reservation, USER_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, 0);
	ksft_test_result(mapping == reservation,
			 "map only the first 4K slice with vm_iomap_memory\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("vm_iomap_memory mmap failed: %s\n",
				   strerror(errno));

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) ||
	    sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	ksft_test_result(read_byte(mapping, &value) && value == FIRST_MARKER,
			 "the requested 4K slice is readable\n");

	for (guard = 1; guard < 4; guard++) {
		if (read_byte(mapping + guard * USER_PAGE_SIZE, &value)) {
			ksft_print_msg("guard %u is readable with value %#x\n",
				       guard, value);
			guards_fault = false;
		}
	}
	ksft_test_result(guards_fault,
			 "vm_iomap_memory does not populate adjacent guard slices\n");
	munmap(reservation, NATIVE_16K_SIZE);

	reservation = mmap(NULL, PRIVATE_RESERVATION_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("private guard reservation failed: %s\n",
				   strerror(errno));
	target_address = ((uintptr_t)reservation + NATIVE_16K_SIZE - 1) &
		~(NATIVE_16K_SIZE - 1);
	mapping = mmap((void *)target_address, USER_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
	ksft_test_result(mapping == (void *)target_address,
			 "map one private PFN-backed process page\n");
	if (mapping != (void *)target_address)
		ksft_exit_fail_msg("private mmap failed: %s\n",
				   strerror(errno));
	mapping_ok = mapping_has_values(mapping, USER_PAGE_SIZE);
	ksft_test_result(mapping_ok,
			 "private PFN mapping initially contains device data\n");
	if (!mapping_ok)
		ksft_exit_fail_msg("private mapping data mismatch\n");
	mapping[COW_OFFSET] = COW_MARKER;
	ksft_test_result(mapping_has_values(mapping, COW_OFFSET),
			 "COW preserves the complete process page\n");
	munmap(reservation, PRIVATE_RESERVATION_SIZE);

	reservation = mmap(NULL, NATIVE_16K_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("offset guard reservation failed: %s\n",
				   strerror(errno));
	mapping = mmap(reservation, USER_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, USER_PAGE_SIZE);
	ksft_test_result(mapping == reservation,
			 "map one 4K slice at physical buffer offset 4K\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("offset mmap failed: %s\n", strerror(errno));
	value = 0;
	ksft_test_result(read_byte(mapping, &value) && value == FIRST_MARKER + 1,
			 "the offset mapping starts at the second 4K slice\n");

	munmap(reservation, NATIVE_16K_SIZE);
	past_end = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			NATIVE_16K_SIZE);
	ksft_test_result(past_end == MAP_FAILED,
			 "reject a 4K mapping starting at the buffer end\n");
	if (past_end != MAP_FAILED)
		munmap(past_end, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "vm_iomap_memory_ppps", "--run", NULL);
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
