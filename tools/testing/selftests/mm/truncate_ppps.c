// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/memfd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAPPING_OFFSET	(3 * USER_PAGE_SIZE)
#define MAPPING_SIZE	(2 * USER_PAGE_SIZE)
#define FILE_SIZE	(8 * USER_PAGE_SIZE)
#define TRUNCATED_SIZE	(4 * USER_PAGE_SIZE)

static sigjmp_buf fault_env;

static unsigned char page_pattern(unsigned int page)
{
	return 0x41 + page;
}

static bool initialize_file(int fd)
{
	unsigned char page[USER_PAGE_SIZE];
	unsigned int i;

	for (i = 0; i < FILE_SIZE / USER_PAGE_SIZE; i++) {
		ssize_t written;

		memset(page, page_pattern(i), sizeof(page));
		written = pwrite(fd, page, sizeof(page), i * USER_PAGE_SIZE);
		if (written != (ssize_t)sizeof(page)) {
			ksft_print_msg("pwrite page %u returned %zd: %s\n", i,
				       written, strerror(errno));
			return false;
		}
	}
	return true;
}

static void sigbus_handler(int signal_number)
{
	siglongjmp(fault_env, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *address;
	return true;
}

static int run_test(void)
{
	unsigned char *mapping;
	struct sigaction action = {
		.sa_handler = sigbus_handler,
	};
	unsigned char truncated_value = 0;
	bool initial_contents;
	bool truncated_ok;
	bool truncated_readable;
	int fd;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = memfd_create("truncate-ppps", MFD_CLOEXEC);
	if (fd < 0)
		ksft_exit_fail_msg("memfd_create failed: %s\n", strerror(errno));
	if (!initialize_file(fd))
		ksft_exit_fail_msg("failed to initialize memfd\n");
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ, MAP_SHARED, fd,
		       MAPPING_OFFSET);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	initial_contents = mapping[0] == page_pattern(3) &&
			   mapping[USER_PAGE_SIZE] == page_pattern(4);
	ksft_test_result(initial_contents,
			 "sliced mapping initially exposes both file pages\n");

	truncated_ok = !ftruncate(fd, TRUNCATED_SIZE);
	ksft_test_result(truncated_ok, "truncate at the native-page boundary succeeds\n");
	if (!truncated_ok)
		ksft_print_msg("ftruncate failed: %s\n", strerror(errno));

	ksft_test_result(mapping[0] == page_pattern(3),
			 "file data below the new EOF remains accessible\n");
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	truncated_readable = read_byte(&mapping[USER_PAGE_SIZE],
				       &truncated_value);
	ksft_test_result(!truncated_readable,
			 "file data at the new EOF is unmapped and faults with SIGBUS\n");
	if (truncated_readable)
		ksft_print_msg("post-EOF access returned %#x\n", truncated_value);

	munmap(mapping, MAPPING_SIZE);
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
	execl("/proc/self/exe", "truncate_ppps", "--run", NULL);
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
