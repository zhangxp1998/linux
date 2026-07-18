// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define BTF_PATH "/sys/kernel/btf/vmlinux"

static sigjmp_buf fault_env;

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

static bool mapping_is_readable(const unsigned char *mapping, size_t size,
				size_t *fault_offset, unsigned int *checksum)
{
	unsigned char value;
	size_t offset;

	for (offset = 0; offset < size; offset += USER_PAGE_SIZE) {
		if (!read_byte(&mapping[offset], &value)) {
			*fault_offset = offset;
			return false;
		}
		*checksum = (*checksum * 33) ^ value;
	}
	offset = size - 1;
	if (!read_byte(&mapping[offset], &value)) {
		*fault_offset = offset;
		return false;
	}
	*checksum = (*checksum * 33) ^ value;
	return true;
}

static int run_test(void)
{
	struct sigaction action = {
		.sa_handler = sigbus_handler,
	};
	const unsigned char *mapping;
	unsigned int checksum = 0;
	struct stat status;
	size_t fault_offset = 0;
	bool readable;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open(BTF_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			ksft_exit_skip("%s is unavailable\n", BTF_PATH);
		ksft_exit_fail_msg("open %s failed: %s\n", BTF_PATH,
				   strerror(errno));
	}
	if (fstat(fd, &status) || status.st_size <= 0)
		ksft_exit_fail_msg("invalid %s size: %s\n", BTF_PATH,
				   strerror(errno));
	ksft_test_result(status.st_size > (off_t)USER_PAGE_SIZE,
			 "BTF exposes a multi-page PFN mapping\n");

	mapping = mmap(NULL, status.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "BTF PFN mapping succeeds\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap %s failed: %s\n", BTF_PATH,
				   strerror(errno));
	close(fd);

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL))
		ksft_exit_fail_msg("sigaction failed: %s\n", strerror(errno));
	readable = mapping_is_readable(mapping, status.st_size, &fault_offset,
				       &checksum);
	if (!readable)
		ksft_print_msg("SIGBUS at file offset %#zx of %#llx\n",
			       fault_offset, (unsigned long long)status.st_size);
	else
		ksft_print_msg("BTF size=%#llx checksum=%#x\n",
			       (unsigned long long)status.st_size, checksum);
	ksft_test_result(readable,
			 "every valid byte range in the PFN mapping is readable\n");

	munmap((void *)mapping, status.st_size);
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
	execl("/proc/self/exe", "remap_pfn_range_ppps", "--run", NULL);
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
